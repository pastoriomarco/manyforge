import { describe, expect, it } from 'vitest';

import { scenarioActionRoute } from './documentStates';

const ready = (pending: boolean) => ({ reason: null, pending });
const blocked = (reason: string, pending: boolean) => ({ reason, pending });

describe('scenarioActionRoute', () => {
  it('routes Apply and Revert through the Deployment whenever it differs, carrying the Program', () => {
    expect(scenarioActionRoute('apply', ready(true), ready(true)).target).toBe('deployment');
    expect(scenarioActionRoute('revert', ready(false), ready(true)).target).toBe('deployment');
  });

  it('uses the Program-only path when only the Program differs', () => {
    const nothing = blocked('Nothing to apply: the Deployment Draft matches Manager Applied.', false);
    expect(scenarioActionRoute('apply', ready(true), nothing)).toEqual({ target: 'program', reason: null });
  });

  it('never applies the Program alone while a blocked Deployment change is pending', () => {
    const route = scenarioActionRoute('apply', ready(true), blocked('Another Deployment operation is in progress.', true));
    expect(route).toEqual({ target: null, reason: 'Another Deployment operation is in progress.' });
  });

  it('discards the whole Scenario when either Draft has unapplied changes', () => {
    const nothing = blocked('Nothing to apply.', false);
    expect(scenarioActionRoute('discard', ready(true), nothing).target).toBe('scenario');
    expect(scenarioActionRoute('discard', nothing, ready(true)).target).toBe('scenario');
    expect(scenarioActionRoute('discard', nothing, nothing)).toEqual({
      target: null, reason: 'Nothing to discard: the Scenario Drafts match Applied.',
    });
  });

  it('names a Scenario-level no-op when nothing differs', () => {
    const nothing = blocked('Nothing to revert: the Saved Program matches Applied.', false);
    expect(scenarioActionRoute('revert', nothing, nothing)).toEqual({
      target: null, reason: 'Nothing to revert: the Saved Scenario matches Applied.',
    });
  });
});
