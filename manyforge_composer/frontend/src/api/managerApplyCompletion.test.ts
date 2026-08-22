import { describe, expect, it, vi } from 'vitest';

import {
  completeManagerApply,
} from './managerApplyCompletion';
import type {
  ManagerApplyRequest,
  ManagerApplyResponse,
} from '../types/api';

function requestFixture(): ManagerApplyRequest {
  return {
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
}

function pendingResponse(): ManagerApplyResponse {
  return {
    apiVersion: 'manyforge.manager_command_response.v1',
    applyGeneration: 1,
    applyId: 'apply-1',
    candidateProjectionRevision: null,
    error: null,
    ok: true,
    replayStatus: 'replayed_exact',
    requestId: 'request-1',
    requestOwnerEpoch: 'manager-owner-1',
    state: 'pending',
  };
}

function succeededResponse(): ManagerApplyResponse {
  return {
    ...pendingResponse(),
    candidateProjectionRevision: {
      retained_bundle_id: 'sha256:' + 'd'.repeat(64),
      graph_revision: 'sha256:' + 'e'.repeat(64),
      scene_revision: {
        scene_id: 'sha256:' + 'f'.repeat(64),
        revision_id: 2,
        content_hash: 'sha256:' + '1'.repeat(64),
      },
    },
    state: 'succeeded',
  };
}

describe('Manager Apply completion observation', () => {
  it('replays one byte-equivalent request until durable success', async () => {
    const request = requestFixture();
    let now = 0;
    const seen: ManagerApplyRequest[] = [];
    const apply = vi.fn(async (body: ManagerApplyRequest) => {
      seen.push(body);
      return seen.length < 3 ? pendingResponse() : succeededResponse();
    });
    const sleep = vi.fn(async (delayMs: number) => {
      now += delayMs;
    });

    const result = await completeManagerApply(request, apply, {
      now: () => now,
      sleep,
      overallTimeoutMs: 4_000,
      replayIntervalMs: 1_000,
    });

    expect(result.state).toBe('succeeded');
    expect(apply).toHaveBeenCalledTimes(3);
    expect(seen.every((body) => body === request)).toBe(true);
    expect(seen.map((body) => JSON.stringify(body))).toEqual([
      JSON.stringify(request),
      JSON.stringify(request),
      JSON.stringify(request),
    ]);
    expect(sleep).toHaveBeenCalledTimes(2);
  });

  it('fails closed when a pending replay changes the durable Apply identity', async () => {
    const request = requestFixture();
    let now = 0;
    let calls = 0;
    const apply = vi.fn(async () => {
      calls += 1;
      return calls === 1
        ? pendingResponse()
        : { ...pendingResponse(), applyId: 'apply-2' };
    });
    const sleep = vi.fn(async (delayMs: number) => {
      now += delayMs;
    });

    await expect(
      completeManagerApply(request, apply, {
        now: () => now,
        sleep,
        overallTimeoutMs: 2_000,
        replayIntervalMs: 1_000,
      }),
    ).rejects.toThrow('durable Apply identity or generation');
    expect(apply).toHaveBeenCalledTimes(2);
  });

  it('fails closed when terminal success changes the durable Apply generation', async () => {
    const request = requestFixture();
    let now = 0;
    let calls = 0;
    const apply = vi.fn(async () => {
      calls += 1;
      return calls === 1
        ? pendingResponse()
        : { ...succeededResponse(), applyGeneration: 2 };
    });
    const sleep = vi.fn(async (delayMs: number) => {
      now += delayMs;
    });

    await expect(
      completeManagerApply(request, apply, {
        now: () => now,
        sleep,
        overallTimeoutMs: 2_000,
        replayIntervalMs: 1_000,
      }),
    ).rejects.toThrow('durable Apply identity or generation');
    expect(apply).toHaveBeenCalledTimes(2);
  });

  it('returns a bounded pending result without claiming success', async () => {
    const request = requestFixture();
    let now = 0;
    const apply = vi.fn(async () => pendingResponse());
    const sleep = vi.fn(async (delayMs: number) => {
      now += delayMs;
    });

    const result = await completeManagerApply(request, apply, {
      now: () => now,
      sleep,
      overallTimeoutMs: 2_500,
      replayIntervalMs: 1_000,
    });

    expect(result.state).toBe('pending_timeout');
    expect(result.response.state).toBe('pending');
    expect(apply.mock.calls.length).toBeLessThanOrEqual(3);
    expect(sleep.mock.calls.map(([delayMs]) => delayMs)).toEqual([
      1_000,
      1_000,
      500,
    ]);
  });

  it('rejects with a pre-aborted signal reason before observing Apply', async () => {
    const request = requestFixture();
    const controller = new AbortController();
    const reason = new Error('Apply observation cancelled before start');
    controller.abort(reason);
    const apply = vi.fn(async () => pendingResponse());
    const sleep = vi.fn(async () => {});

    await expect(
      completeManagerApply(request, apply, {
        signal: controller.signal,
        sleep,
      }),
    ).rejects.toBe(reason);
    expect(apply).not.toHaveBeenCalled();
    expect(sleep).not.toHaveBeenCalled();
  });
});
