import { describe, expect, it } from 'vitest';

import { startupScenarioNotices } from './startupScenarioNotice';

describe('refused startup project notice', () => {
  const unconfirmed = {
    path: '/p/cell.scenario', code: 'scenario_load_activation_not_confirmed',
    message: 'Startup project /p/cell.scenario was opened in the workspace, but its activation was not confirmed.',
  };

  it('shows exactly what health reports, with the startup project open', () => {
    const [notice] = startupScenarioNotices(unconfirmed);
    expect(notice).toMatchObject({
      severity: 'warning', title: 'Startup project opened; its activation was not confirmed.',
      message: unconfirmed.message,
    });
  });

  it('says not opened for a refusal before the workspace commit', () => {
    expect(startupScenarioNotices({ ...unconfirmed, code: 'scenario_new_required', message: 'm' })[0]?.title)
      .toBe('Startup project was not opened.');
    expect(startupScenarioNotices(null)).toEqual([]);
  });
});
