import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';

import { NoticeRow } from '../components/WorkbenchNotifications';
import { resetApplyNotice } from '../hooks/useResetApply';
import type { OperatorAlarmRecord } from '../types/headless';
import { activationFailureNotice } from './activationNotices';
import type { ResetApplyPlan, ResetApplySequence } from './resetApply';
import {
  acknowledgeActions,
  alarmWarningNotices,
  composerWarningNotices,
  locallyAcknowledgedWarnings,
  resetReachedIdle,
  viewerConditionWarning,
} from './warningNotices';
import type { WorkbenchNotice } from './workbenchNotices';

const labels = (notice: WorkbenchNotice | undefined) => notice?.actions?.map((action) => action.label);

describe('warnings: what blocks nothing stays until acknowledged', () => {
  it('shows a Composer request failure as a warning acknowledged in the registry', () => {
    const dismiss = vi.fn();
    const [notice] = composerWarningNotices([{
      id: 'composer.program_runtime:3', title: 'Program Run, Stop, or Reset failed',
      message: 'Program execution failed.', active: false, acknowledgementRequired: true,
      severity: 'warning',
    }], dismiss);
    expect(notice).toMatchObject({ severity: 'warning', title: 'Program Run, Stop, or Reset failed' });
    expect(labels(notice)).toEqual(['Acknowledge']);
    notice?.actions?.[0]?.run();
    expect(dismiss).toHaveBeenCalledWith('composer.program_runtime:3');
  });

  it('keeps a stopped Apply until acknowledged, then stops following it', () => {
    const dismiss = vi.fn();
    const acknowledge = vi.fn();
    const stopped = resetApplyNotice({
      plan: { correlationId: 'corr-1', document: 'program' } as unknown as ResetApplyPlan,
      phase: { kind: 'failed', stage: 'apply', message: 'refused', unknown: false, apply: null },
    } as ResetApplySequence, dismiss);
    const [shown] = locallyAcknowledgedWarnings([stopped], new Set(), acknowledge);
    expect(shown?.severity).toBe('warning');
    expect(labels(shown)).toEqual(['Acknowledge']);
    shown?.actions?.[0]?.run();
    expect(acknowledge).toHaveBeenCalledWith('reset-apply.corr-1');
    expect(dismiss).toHaveBeenCalledOnce();
    expect(locallyAcknowledgedWarnings([stopped], new Set([stopped.id]), acknowledge)).toEqual([]);
  });

  it('shows a refused Activation that changed nothing as an acknowledgeable warning', () => {
    const notice = activationFailureNotice('activation.b:1', 'apply.b', {
      state: 'failed',
      failure: {
        code: 'manager_apply_preflight_refused', message: 'Candidate preflight was refused.',
        phase: 'accepted', effectDisposition: 'known',
      },
    }, null, true);
    const [shown] = locallyAcknowledgedWarnings([notice], new Set(), vi.fn());
    expect(shown?.severity).toBe('warning');
    expect(labels(shown)).toEqual(['Acknowledge']);
  });

  it('shows a viewer condition as a warning that keeps its Reload action', () => {
    const reload = vi.fn();
    const viewer = viewerConditionWarning({
      id: 'viewer.asset', severity: 'error', domain: 'viewer', title: 'Robot geometry unavailable',
      message: 'mesh failed', incidentKind: 'viewer_geometry_unavailable',
      actions: [{ id: 'reload-active-model', label: 'Reload active model', run: reload }],
    });
    const [shown] = locallyAcknowledgedWarnings([viewer], new Set(), vi.fn());
    expect(shown?.severity).toBe('warning');
    expect(labels(shown)).toEqual(['Reload active model', 'Acknowledge']);
  });

  it('shows a non-gating alarm as a warning with the exact server acknowledgement', () => {
    const alarm = {
      conditionId: 'carried', activationId: 'carried.1', recordRevision: 2,
      title: 'Carried object lost', message: 'The gripper lost the object.',
      consequence: '', operatorResponse: 'Inspect the cell.',
    } as unknown as OperatorAlarmRecord;
    const acknowledge = vi.fn();
    const [notice] = alarmWarningNotices([alarm], null, acknowledge);
    expect(notice).toMatchObject({
      id: 'alarm.carried.1', severity: 'warning', title: 'Carried object lost',
      details: 'Inspect the cell.',
    });
    notice?.actions?.[0]?.run();
    expect(acknowledge).toHaveBeenCalledWith(alarm);
    expect(alarmWarningNotices([alarm], 'carried.1', acknowledge)[0]?.actions?.[0]?.disabled)
      .toBe(true);
  });

  it('acknowledges every warning at once without confirmation', () => {
    const first = vi.fn();
    const second = vi.fn();
    const notices = [
      ...composerWarningNotices([
        { id: 'a', title: 'A', message: 'a', active: true, acknowledgementRequired: true, severity: 'warning' },
        { id: 'b', title: 'B', message: 'b', active: true, acknowledgementRequired: true, severity: 'warning' },
      ], (id) => (id === 'a' ? first() : second())),
      // A source action other than Acknowledge is never run in bulk.
      { id: 'other.warning', severity: 'warning' as const, domain: 'runtime' as const,
        title: 'Other', message: 'other',
        actions: [{ id: 'dismiss', label: 'Dismiss', run: vi.fn() }] },
    ];
    const all = acknowledgeActions(notices);
    expect(all).toHaveLength(2);
    all.forEach((action) => action.run());
    expect(first).toHaveBeenCalledOnce();
    expect(second).toHaveBeenCalledOnce();
    const row = renderToStaticMarkup(createElement(NoticeRow, { severity: 'warning', notices }));
    expect(row).toContain('>Acknowledge all</button>');
    expect(row).not.toContain('Confirm');
  });

  it('clears warnings when a Reset reaches Idle, not on page load or while Idle', () => {
    expect(resetReachedIdle('resetting', 'idle_after_reset')).toBe(true);
    expect(resetReachedIdle('terminal_awaiting_reset', 'idle_after_reset')).toBe(true);
    expect(resetReachedIdle(undefined, 'idle_after_reset')).toBe(false);
    expect(resetReachedIdle('idle_after_reset', 'idle_after_reset')).toBe(false);
    expect(resetReachedIdle('idle_after_reset', 'running')).toBe(false);
  });
});
