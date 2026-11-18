import type { ApplyCommandRow, ApplyCommandRowKind, JsonValue } from '../types/api';
import type { ApplyResult } from '../types/components';

let nextKey = 1;

/**
 * An Apply-level command's row as the reply carries it when the command had
 * already ended (OD-214), stating `result`: the end's code is the ApplyResult
 * error's code and its details carry the component rows.
 */
export function endedApplyRow(
  result: ApplyResult,
  kind: ApplyCommandRowKind = 'deployment_apply',
): ApplyCommandRow {
  const requestKey = (nextKey++).toString(16).padStart(32, '0');
  return {
    kind,
    requestKey,
    step: 'preparing_components',
    stepSource: null,
    acceptedAtNs: 1,
    end: {
      outcome: result.outcome,
      code: result.outcome === 'succeeded' ? null : result.error?.code ?? 'apply_failed',
      details: { components: result.components as unknown as JsonValue },
      finishedAtNs: 2,
    },
  };
}

/**
 * A Program Apply or Revert row that ended succeeded; `managerApply` is the
 * Manager Apply its `applying` step started (none when Applied already matched).
 */
export function endedProgramApplyRow(
  kind: 'program_apply' | 'program_revert',
  managerApply: { applyId: number; applyGeneration: number } | null,
): ApplyCommandRow {
  return {
    kind,
    requestKey: (nextKey++).toString(16).padStart(32, '0'),
    step: managerApply === null ? 'previewing' : 'applying',
    stepSource: managerApply === null ? null : { status: 'manager_apply', ...managerApply },
    acceptedAtNs: '1',
    end: { outcome: 'succeeded', code: null, details: null, finishedAtNs: '2' },
  };
}
