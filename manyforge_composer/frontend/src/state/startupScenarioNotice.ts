import type { StartupScenarioNotice } from '../types/api';
import type { WorkbenchNotice } from './workbenchNotices';

/**
 * The warning for a refused startup project, exactly while the backend's
 * health reports it. Only the backend dismisses it (at a successful Load or
 * New commit); whether a project is open is no evidence.
 */
export function startupScenarioNotices(notice: StartupScenarioNotice | null): WorkbenchNotice[] {
  if (notice === null) return [];
  return [{
    id: `startup-scenario.${notice.path}`,
    severity: 'warning',
    domain: 'recovery',
    title: notice.code === 'scenario_load_activation_not_confirmed'
      ? 'Startup project opened; its activation was not confirmed.'
      : 'Startup project was not opened.',
    message: notice.message,
  }];
}
