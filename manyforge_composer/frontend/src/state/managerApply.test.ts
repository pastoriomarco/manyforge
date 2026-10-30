import { describe, expect, it } from 'vitest';

import {
  acceptManagerApplyAcknowledgementStatus,
  acceptManagerApplyStatus,
  selectManagerApplyInventoryIdentity,
  selectManagerApplyObservationIdentity,
} from './managerApply';
import type {
  ManagerApplyLifecyclePhase,
  ManagerApplyRecord,
  ManagerApplyStatus,
} from '../types/api';

function status(
  phase: ManagerApplyLifecyclePhase,
  state: ManagerApplyStatus['state'] = 'pending',
): ManagerApplyStatus {
  const failed = state === 'failed' || state === 'unknown_effect';
  return {
    apiVersion: 'manyforge.manager_command_response.v1',
    applyGeneration: 2,
    applyId: 'apply-2',
    candidateProjectionRevision: state === 'succeeded' ? {
      retained_bundle_id: `sha256:${'1'.repeat(64)}`,
      graph_revision: `sha256:${'2'.repeat(64)}`,
      scene_revision: {
        scene_id: `sha256:${'3'.repeat(64)}`,
        revision_id: 4,
        content_hash: `sha256:${'4'.repeat(64)}`,
      },
    } : null,
    error: null,
    failure: failed ? {
      code: 'runtime_initialization_failed',
      message: 'replacement initialization failed',
      phase,
      effectDisposition: state === 'failed' ? 'known' : 'unknown',
      causeCode: 'typed_cause_code',
      causeDetails: {},
      producer: 'component_manager',
      effectBoundary: 'unknown',
    } : null,
    lastProgress: {
      phase,
      recordDigest: `sha256:${'5'.repeat(64)}`,
    },
    ok: true,
    phase,
    sceneInitialization: 'preserve',
    preparedActivation: {
      capturedInputDigest: `sha256:${'6'.repeat(64)}`,
      deploymentArtifactId: 'deployment-1',
      deploymentRevision: `sha256:${'7'.repeat(64)}`,
      programBindingId: `sha256:${'8'.repeat(64)}`,
      preparedActivationId: `sha256:${'9'.repeat(64)}`,
    },
    requestId: 'request-1',
    requestOwnerEpoch: 'owner-1',
    state,
    failureAcknowledgement: 'unavailable',
  };
}

describe('Manager Apply presentation state', () => {
  it('selects an incomplete operation, then a retained terminal failure', () => {
    const record = (
      applyId: string,
      applyGeneration: number,
      incomplete: boolean,
      terminalState: ManagerApplyRecord['terminalState'],
    ): ManagerApplyRecord => ({
      ...({} as ManagerApplyRecord),
      applyId,
      applyGeneration,
      incomplete,
      terminalState,
    });
    expect(selectManagerApplyInventoryIdentity([
      record('old-failure', 9, false, 'failed'),
      record('current', 1, true, null),
    ])).toEqual({ applyId: 'current', applyGeneration: 1 });
    expect(selectManagerApplyInventoryIdentity([
      record('success', 20, false, 'succeeded'),
      record('failed', 4, false, 'unknown_effect'),
    ])).toEqual({ applyId: 'failed', applyGeneration: 4 });
    expect(selectManagerApplyInventoryIdentity([
      record('success', 20, false, 'succeeded'),
    ])).toBeNull();
  });

  it('advances pending phases and suppresses a delayed stale observation', () => {
    const advanced = acceptManagerApplyStatus(
      status('fence_accepted'),
      status('runtime_converged'),
    );
    expect(advanced?.phase).toBe('runtime_converged');
    expect(acceptManagerApplyStatus(advanced, status('accepted'))?.phase)
      .toBe('runtime_converged');
  });

  it('retains terminal failure across stale reconnect observations', () => {
    const terminal = status('runtime_converged', 'unknown_effect');
    expect(acceptManagerApplyStatus(terminal, status('accepted'))).toBe(terminal);
    expect(terminal.failure?.effectDisposition).toBe('unknown');
  });

  it('accepts explicit acknowledgement completion without letting stale status undo it', () => {
    const offered = {
      ...status('runtime_converged', 'failed'),
      failure: {
        ...status('runtime_converged', 'failed').failure!,
        effectBoundary: 'before_effect' as const,
      },
      failureAcknowledgement: 'available' as const,
    };
    const completed = {
      ...status('accepted', 'failed'),
      failure: {
        ...status('accepted', 'failed').failure!,
        effectBoundary: 'before_effect' as const,
      },
      failureAcknowledgement: 'acknowledged' as const,
    };
    const acknowledged = acceptManagerApplyAcknowledgementStatus(offered, completed);

    expect(acknowledged?.state).toBe('failed');
    expect(acknowledged?.phase).toBe('runtime_converged');
    expect(acknowledged?.failure).toEqual(offered.failure);
    expect(acknowledged?.failureAcknowledgement).toBe('acknowledged');
    expect(acceptManagerApplyStatus(acknowledged, offered)).toBe(acknowledged);
    expect(acceptManagerApplyAcknowledgementStatus(acknowledged, offered))
      .toBe(acknowledged);
    expect(acceptManagerApplyStatus(offered, completed)).toBe(offered);
  });

  it('keeps unavailable failed acknowledgement active and does not restore stale availability', () => {
    const available = {
      ...status('runtime_converged', 'failed'),
      failure: {
        ...status('runtime_converged', 'failed').failure!,
        effectBoundary: 'before_effect' as const,
      },
      failureAcknowledgement: 'available' as const,
    };
    const unavailable = {
      ...available,
      failureAcknowledgement: 'unavailable' as const,
    };
    expect(acceptManagerApplyAcknowledgementStatus(available, unavailable)?.failureAcknowledgement)
      .toBe('unavailable');
    expect(acceptManagerApplyAcknowledgementStatus(unavailable, available))
      .toBe(unavailable);
  });

  it('does not replace one operation with a response for another identity', () => {
    const current = status('fence_acquired');
    const another = { ...status('settled', 'succeeded'), applyId: 'apply-3' };
    expect(acceptManagerApplyStatus(current, another)).toBe(current);
  });

  it('reconnects to the advertised operation and takes one final observation', () => {
    const pending = status('scene_staged');
    expect(selectManagerApplyObservationIdentity({
      applyId: pending.applyId,
      applyGeneration: pending.applyGeneration,
    }, null)).toEqual({ applyId: 'apply-2', applyGeneration: 2 });
    expect(selectManagerApplyObservationIdentity(null, pending)).toEqual({
      applyId: 'apply-2',
      applyGeneration: 2,
    });
    expect(selectManagerApplyObservationIdentity(
      null,
      status('settled', 'succeeded'),
    )).toBeNull();
    expect(selectManagerApplyObservationIdentity(
      { applyId: 'apply-2', applyGeneration: 2 },
      status('settled', 'succeeded'),
    )).toBeNull();
  });
});
