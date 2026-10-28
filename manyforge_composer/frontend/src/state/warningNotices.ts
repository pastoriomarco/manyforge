/**
 * The warning row: everything that blocks nothing. Each item stays until the
 * operator acknowledges it, or until Reset or Scenario Close clears it.
 * Composer-side warnings are acknowledged locally (per browser session);
 * alarms through the owner's exact acknowledgement. Red stays for what blocks
 * now (see `HeadlessRuntimeSurfaces`).
 */

import type { ProgramCycleStatus } from '../types/api';
import type { OperatorAlarmRecord } from '../types/headless';
import type { ClientOperationalError } from './operationalErrors';
import type { WorkbenchNotice } from './workbenchNotices';

/** The action id every acknowledgeable warning carries. */
export const ACKNOWLEDGE_ACTION_ID = 'acknowledge';

/** The enabled Acknowledge actions of a row: "Acknowledge all" runs each,
 * without confirmation, since none of them blocks anything. */
export function acknowledgeActions(notices: readonly WorkbenchNotice[]) {
  return notices.flatMap((notice) => notice.actions
    ?.filter((action) => action.id === ACKNOWLEDGE_ACTION_ID && !action.disabled) ?? []);
}

/** A viewer condition blocks no command: a warning while it lasts. */
export function viewerConditionWarning(notice: WorkbenchNotice): WorkbenchNotice {
  return { ...notice, severity: 'warning', acknowledgeable: true };
}

/**
 * Hide the locally acknowledged warnings and give the others Acknowledge,
 * which also runs their own `acknowledge` action (e.g. stop following).
 */
export function locallyAcknowledgedWarnings(
  notices: readonly WorkbenchNotice[],
  acknowledged: ReadonlySet<string>,
  acknowledge: (id: string) => void,
): WorkbenchNotice[] {
  return notices
    .filter((notice) => notice.acknowledgeable !== true || !acknowledged.has(notice.id))
    .map((notice) => {
      if (notice.acknowledgeable !== true) return notice;
      const own = notice.actions?.find((action) => action.id === ACKNOWLEDGE_ACTION_ID);
      return {
        ...notice,
        actions: [
          ...(notice.actions ?? []).filter((action) => action.id !== ACKNOWLEDGE_ACTION_ID),
          {
            id: ACKNOWLEDGE_ACTION_ID,
            label: 'Acknowledge',
            title: own?.title ?? 'Acknowledge this warning. It blocks nothing; this does not change execution.',
            run: () => {
              acknowledge(notice.id);
              own?.run();
            },
          },
        ],
      };
    });
}

/** Composer request failures: kept by the registry until acknowledged. */
export function composerWarningNotices(
  records: readonly ClientOperationalError[],
  dismiss: (id: string) => void,
): WorkbenchNotice[] {
  return records.map((record) => ({
    id: `composer.${record.id}`,
    severity: 'warning',
    domain: 'runtime',
    title: record.title,
    message: record.message,
    actions: [{
      id: ACKNOWLEDGE_ACTION_ID,
      label: 'Acknowledge',
      title: 'Acknowledge this Composer message. This does not change execution.',
      run: () => dismiss(record.id),
    }],
  }));
}

/** Unacknowledged alarms that gate nothing, with the exact acknowledgement. */
export function alarmWarningNotices(
  alarms: readonly OperatorAlarmRecord[],
  busyActivation: string | null,
  acknowledge: (alarm: OperatorAlarmRecord) => void,
): WorkbenchNotice[] {
  return alarms.map((alarm) => {
    const busy = busyActivation === alarm.activationId;
    return {
      id: `alarm.${alarm.activationId}`,
      severity: 'warning',
      domain: 'runtime',
      title: alarm.title,
      message: alarm.message,
      details: [alarm.consequence, alarm.operatorResponse].filter(Boolean).join(' '),
      busy,
      actions: [{
        id: ACKNOWLEDGE_ACTION_ID,
        label: 'Acknowledge',
        title: 'Acknowledge this exact alarm activation and revision. It blocks nothing; this does not change execution.',
        disabled: busy,
        run: () => acknowledge(alarm),
      }],
    };
  });
}

/** A Reset reached Idle: the cycle entered `idle_after_reset` from another state. */
export function resetReachedIdle(
  previous: ProgramCycleStatus['state'] | undefined,
  current: ProgramCycleStatus['state'] | undefined,
): boolean {
  return current === 'idle_after_reset' && previous !== undefined
    && previous !== 'idle_after_reset';
}
