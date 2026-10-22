import { describe, expect, it, vi } from 'vitest';

import type { ResetApplyPlan, ResetApplySequence } from '../state/resetApply';
import { resetApplyNotice } from './useResetApply';

const plan = { correlationId: 'corr-1', document: 'deployment' } as unknown as ResetApplyPlan;

describe('Reset-then-Apply Activity notice', () => {
  it('reports a completed Apply as a finished result without Dismiss', () => {
    const notice = resetApplyNotice(
      { plan, phase: { kind: 'applied', apply: null } } as ResetApplySequence, vi.fn(),
    );
    expect(notice).toMatchObject({ severity: 'info', busy: false, title: 'Deployment Applied after Reset' });
    expect(notice.actions).toBeUndefined();
  });

  it('keeps the acknowledgement of a stopped Apply and shows progress while Resetting', () => {
    const stopped = resetApplyNotice({ plan, phase: {
      kind: 'failed', stage: 'apply', message: 'refused', unknown: false, apply: null,
    } } as ResetApplySequence, vi.fn());
    expect(stopped.severity).toBe('error');
    expect(stopped.actions?.map((action) => action.label)).toEqual(['Dismiss']);
    const resetting = resetApplyNotice({ plan, phase: { kind: 'resetting' } } as ResetApplySequence, vi.fn());
    expect(resetting.busy).toBe(true);
  });
});
