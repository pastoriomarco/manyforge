import { describe, expect, it } from 'vitest';

import { activationFailureNotice } from './activationNotices';

describe('activation failure notice', () => {
  it('keeps the owner failure phase, code and effect disposition', () => {
    const notice = activationFailureNotice(
      'activation.a:1',
      'apply.a',
      {
        state: 'unknown_effect',
        failure: {
          code: 'runtime_composition_publication_verification_failed',
          message: 'native CCLC candidate routes are not ready.',
          phase: 'runtime_converged',
          effectDisposition: 'unknown',
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
      'Close and preserve evidence: ./scripts/compose-dev.sh stop --keep-evidence',
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
    expect(notice.message).toContain('Execution never resumes automatically');
  });

  it('does not prescribe recovery for a known pre-effect refusal', () => {
    const notice = activationFailureNotice('activation.b:1', 'apply.b', {
      state: 'failed',
      failure: {
        code: 'manager_apply_preflight_refused',
        message: 'Candidate preflight was refused.',
        phase: 'accepted',
        effectDisposition: 'known',
      },
    }, null, true);
    expect(notice).toMatchObject({
      title: 'Activation failed',
    });
    expect(notice.message).toContain(
      'Candidate preflight was refused. Failed during accepted (manager_apply_preflight_refused); its effects are known.',
    );
    expect(notice.message).toContain('Manager recorded a known pre-effect refusal');
    expect(notice.message).toContain('current owner confirms authoring admission is open');
    expect(notice.message).toContain('cold restart is not required');
    expect(notice.message).toContain('Correct the reported cause before trying again');
    expect(notice.message).not.toContain('stop --keep-evidence');
    expect(notice.message).not.toContain('start --project');
  });

  it('keeps cold recovery for a known failure after the pre-effect phase', () => {
    const notice = activationFailureNotice('activation.c:1', 'apply.c', {
      state: 'failed',
      failure: {
        code: 'runtime_convergence_failed',
        message: 'Runtime convergence failed.',
        phase: 'runtime_converged',
        effectDisposition: 'known',
      },
    }, '/tmp/external.scenario', false);

    expect(notice.message).toContain('The runtime remains locked');
    expect(notice.message).toContain(
      'The Saved Scenario is outside the mounted repository, so a new workbench cannot reload it; only Close is available.',
    );
    expect(notice.message).toContain('./scripts/compose-dev.sh stop --keep-evidence');
    expect(notice.message).not.toContain('start --project');
    expect(notice.message).toMatch(
      /Close and preserve evidence: \.\/scripts\/compose-dev\.sh stop --keep-evidence$/,
    );
  });

  it.each([
    '/workspace/scenarios/cell.scenario.yaml',
    '/workspace/scenarios/cell.scenario.yml',
  ])('offers only Close for single-file Saved Scenario %s', (scenarioPath) => {
    const notice = activationFailureNotice('activation.d:1', 'apply.d', {
      state: 'unknown_effect',
      failure: {
        code: 'scene_publication_unknown',
        message: 'Scene publication could not be confirmed.',
        phase: 'scene_activated',
        effectDisposition: 'unknown',
      },
    }, scenarioPath, false);

    expect(notice.message).toContain(
      'The Saved Scenario is a single-file artifact, while this workbench start command accepts Scenario project directories; only Close is available.',
    );
    expect(notice.message).toContain('./scripts/compose-dev.sh stop --keep-evidence');
    expect(notice.message).not.toContain('start --project');
  });

  it('fails closed when a terminal failure has no detail', () => {
    const notice = activationFailureNotice('activation.e:1', 'apply.e', {
      state: 'failed', failure: null,
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
    const notice = activationFailureNotice('activation.f:1', 'apply.f', {
      state: 'failed',
      failure: {
        code: 'manager_apply_preflight_refused',
        message: 'Candidate preflight was refused.',
        phase: 'accepted',
        effectDisposition: 'known',
      },
    }, '/workspace/scenarios/cell.scenario', false);

    expect(notice.message).toContain('current owner still reports authoring admission closed');
    expect(notice.message).toContain('Do not retry while it remains closed');
    expect(notice.message).toContain('Apply record alone does not justify a cold restart');
    expect(notice.message).not.toContain('stop --keep-evidence');
  });

  it('does not infer settlement when authoring admission is unavailable', () => {
    const notice = activationFailureNotice('activation.g:1', 'apply.g', {
      state: 'failed',
      failure: {
        code: 'manager_apply_preflight_refused',
        message: 'Candidate preflight was refused.',
        phase: 'accepted',
        effectDisposition: 'known',
      },
    }, '/workspace/scenarios/cell.scenario', null);

    expect(notice.message).toContain('Current authoring admission is unavailable');
    expect(notice.message).toContain('settlement cannot be confirmed');
    expect(notice.message).not.toContain('stop --keep-evidence');
  });
});
