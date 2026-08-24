import { describe, expect, it, vi } from 'vitest';

import {
  completeManagerApply,
  observePendingManagerApply,
} from './managerApplyCompletion';
import type {
  ManagerApplyPendingResponse,
  ManagerApplyRequest,
  ManagerApplyStatusResponse,
} from '../types/api';

const request: ManagerApplyRequest = {
  request_id: 'request-1',
  request_owner_epoch: 'manager-owner-1',
  source_label: 'composer_ui',
  correlation_id: null,
  deployment_artifact_id: 'deployment-1',
  deployment_revision: 'sha256:' + 'a'.repeat(64),
  expected_current: {
    graph_revision: 'sha256:' + 'b'.repeat(64),
    retained_bundle_id: 'sha256:' + 'c'.repeat(64),
  },
  reject_if_affected_busy: true,
};

function pending(): ManagerApplyPendingResponse {
  return {
    apiVersion: 'manyforge.manager_command_response.v1',
    applyGeneration: 1,
    applyId: 'apply-1',
    candidateProjectionRevision: null,
    error: null,
    ok: true,
    replayStatus: 'accepted_new',
    requestId: request.request_id,
    requestOwnerEpoch: request.request_owner_epoch,
    state: 'pending',
  };
}

function status(state: 'pending' | 'succeeded'): ManagerApplyStatusResponse {
  return state === 'pending'
    ? {
        apiVersion: 'manyforge.manager_command_response.v1',
        applyGeneration: 1,
        applyId: 'apply-1',
        candidateProjectionRevision: null,
        error: null,
        ok: true,
        state,
      }
    : {
        apiVersion: 'manyforge.manager_command_response.v1',
        applyGeneration: 1,
        applyId: 'apply-1',
        candidateProjectionRevision: {
          retained_bundle_id: 'sha256:' + 'd'.repeat(64),
          graph_revision: 'sha256:' + 'e'.repeat(64),
          scene_revision: {
            scene_id: 'sha256:' + 'f'.repeat(64),
            revision_id: 2,
            content_hash: 'sha256:' + '1'.repeat(64),
          },
        },
        error: null,
        ok: true,
        state,
      };
}

describe('Manager Apply durable observation', () => {
  it('submits once and observes the durable identity until success', async () => {
    let now = 0;
    const submit = vi.fn(async () => pending());
    const observe = vi
      .fn<(_: string, __: number) => Promise<ManagerApplyStatusResponse>>()
      .mockResolvedValueOnce(status('pending'))
      .mockResolvedValueOnce(status('succeeded'));
    const result = await completeManagerApply(request, submit, observe, {
      now: () => now,
      sleep: async (duration) => { now += duration; },
      overallTimeoutMs: 4_000,
      observationIntervalMs: 1_000,
    });
    expect(result.state).toBe('succeeded');
    expect(submit).toHaveBeenCalledTimes(1);
    expect(observe).toHaveBeenCalledTimes(2);
    expect(observe).toHaveBeenCalledWith('apply-1', 1, undefined);
  });

  it('times out pending without resubmission or no-effect inference', async () => {
    let now = 0;
    const submit = vi.fn(async () => pending());
    const observe = vi.fn(async () => status('pending'));
    const result = await completeManagerApply(request, submit, observe, {
      now: () => now,
      sleep: async (duration) => { now += duration; },
      overallTimeoutMs: 2_500,
      observationIntervalMs: 1_000,
    });
    expect(result.state).toBe('pending_timeout');
    expect(submit).toHaveBeenCalledTimes(1);
    expect(observe).toHaveBeenCalledTimes(2);
  });

  it('pre-abort dispatches neither submit nor observation', async () => {
    const controller = new AbortController();
    const reason = new Error('local observer closed');
    controller.abort(reason);
    const submit = vi.fn(async () => pending());
    const observe = vi.fn(async () => status('pending'));
    await expect(
      completeManagerApply(request, submit, observe, {
        signal: controller.signal,
      }),
    ).rejects.toBe(reason);
    expect(submit).not.toHaveBeenCalled();
    expect(observe).not.toHaveBeenCalled();
  });

  it('abort during delay stops only the local observer', async () => {
    const controller = new AbortController();
    const submit = vi.fn(async () => pending());
    const observe = vi.fn(async () => status('pending'));
    await expect(
      completeManagerApply(request, submit, observe, {
        signal: controller.signal,
        sleep: async () => {
          controller.abort(new Error('unmounted'));
          throw controller.signal.reason;
        },
      }),
    ).rejects.toThrow('unmounted');
    expect(submit).toHaveBeenCalledTimes(1);
    expect(observe).not.toHaveBeenCalled();
  });

  it('reconnects from a durable receipt using observation only', async () => {
    let now = 0;
    const observe = vi.fn(async () => status('succeeded'));
    const result = await observePendingManagerApply(pending(), observe, {
      now: () => now,
      sleep: async (duration) => { now += duration; },
    });

    expect(result.state).toBe('succeeded');
    expect(observe).toHaveBeenCalledTimes(1);
    expect(observe).toHaveBeenCalledWith('apply-1', 1, undefined);
  });

  it('abort during status transport does not resubmit', async () => {
    const controller = new AbortController();
    const submit = vi.fn(async () => pending());
    const observe = vi.fn(async () => {
      const reason = new Error('observer disconnected');
      controller.abort(reason);
      throw reason;
    });
    await expect(completeManagerApply(request, submit, observe, {
      signal: controller.signal,
      sleep: async () => undefined,
    })).rejects.toThrow('observer disconnected');
    expect(submit).toHaveBeenCalledTimes(1);
    expect(observe).toHaveBeenCalledTimes(1);
  });
});
