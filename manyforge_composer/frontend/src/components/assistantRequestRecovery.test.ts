import { describe, expect, it } from 'vitest';

import { ApiError } from '../api/client';
import type { AssistantRequestStatus } from '../types/api';
import {
  reconcileAssistantRequest,
  shouldReconcileAssistantRequest,
} from './assistantRequestRecovery';

function status(
  state: AssistantRequestStatus['status'],
  overrides: Partial<AssistantRequestStatus> = {},
): AssistantRequestStatus {
  return {
    requestId: 'request-1',
    conversationId: 'conversation-1',
    providerId: 'openclaw',
    assistantMode: 'composer-assistant',
    status: state,
    message: state === 'completed' ? 'Draft updated.' : '',
    error: null,
    warnings: [],
    draftMutated: state === 'completed',
    toolCallCount: 1,
    recentToolCalls: [],
    startedAtNs: 1,
    updatedAtNs: 2,
    finishedAtNs: state === 'running' ? null : 2,
    ...overrides,
  };
}

const noWait = async () => undefined;

describe('assistant request reconciliation', () => {
  it('reconciles transport and server failures, but not definitive client errors', () => {
    expect(shouldReconcileAssistantRequest(
      new TypeError('NetworkError when attempting to fetch resource.'),
    )).toBe(true);
    expect(shouldReconcileAssistantRequest(
      new ApiError('Internal Server Error', 500, null),
    )).toBe(true);
    expect(shouldReconcileAssistantRequest(
      new ApiError('Invalid assistant request', 422, null),
    )).toBe(false);
  });

  it('waits for a running request and returns its authoritative completion', async () => {
    const responses = [status('running'), status('completed')];

    const result = await reconcileAssistantRequest({
      readStatus: async () => responses.shift() ?? status('completed'),
      wait: noWait,
    });

    expect(result).toEqual({ kind: 'terminal', status: status('completed') });
  });

  it('tolerates initial 404 responses so a status read may not overtake acceptance', async () => {
    let attempts = 0;

    const result = await reconcileAssistantRequest({
      readStatus: async () => {
        attempts += 1;
        if (attempts < 3) {
          throw { status: 404 };
        }
        return status('completed');
      },
      maxNotFoundAttempts: 3,
      wait: noWait,
    });

    expect(result.kind).toBe('terminal');
    expect(attempts).toBe(3);
  });

  it('reports an unknown outcome after repeated missing status records', async () => {
    let attempts = 0;

    const result = await reconcileAssistantRequest({
      readStatus: async () => {
        attempts += 1;
        throw { status: 404 };
      },
      maxNotFoundAttempts: 3,
      wait: noWait,
    });

    expect(result).toEqual({ kind: 'not_found', requestObserved: false });
    expect(attempts).toBe(3);
  });

  it('remembers when a request was observed before its status record disappears', async () => {
    let attempts = 0;

    const result = await reconcileAssistantRequest({
      readStatus: async () => {
        attempts += 1;
        if (attempts === 1) {
          return status('running');
        }
        throw { status: 404 };
      },
      maxNotFoundAttempts: 2,
      wait: noWait,
    });

    expect(result).toEqual({ kind: 'not_found', requestObserved: true });
  });

  it('recovers from transient read failures and returns the server failure', async () => {
    let attempts = 0;
    const failed = status('failed', {
      message: 'Provider failed.',
      error: { code: 'upstream_call_error', detail: 'Provider failed.' },
      draftMutated: false,
    });

    const result = await reconcileAssistantRequest({
      readStatus: async () => {
        attempts += 1;
        if (attempts < 3) {
          throw new TypeError('NetworkError when attempting to fetch resource.');
        }
        return failed;
      },
      maxReadFailures: 3,
      wait: noWait,
    });

    expect(result).toEqual({ kind: 'terminal', status: failed });
  });

  it('reports an unreachable outcome after repeated status read failures', async () => {
    const result = await reconcileAssistantRequest({
      readStatus: async () => {
        throw new TypeError('NetworkError when attempting to fetch resource.');
      },
      maxReadFailures: 3,
      wait: noWait,
    });

    expect(result).toEqual({
      kind: 'unreachable',
      detail: 'NetworkError when attempting to fetch resource.',
    });
  });

  it('reports an unknown outcome when a request remains non-terminal too long', async () => {
    let attempts = 0;

    const result = await reconcileAssistantRequest({
      readStatus: async () => {
        attempts += 1;
        return status('running');
      },
      maxPollAttempts: 3,
      wait: noWait,
    });

    expect(result).toEqual({ kind: 'exhausted' });
    expect(attempts).toBe(3);
  });

  it('stops without producing an outcome when recovery is aborted', async () => {
    const controller = new AbortController();
    controller.abort();

    const result = await reconcileAssistantRequest({
      readStatus: async () => status('completed'),
      signal: controller.signal,
      wait: noWait,
    });

    expect(result).toEqual({ kind: 'aborted' });
  });

  it('lets an abort win over a terminal response already being read', async () => {
    const controller = new AbortController();
    let resolveRead!: (value: AssistantRequestStatus) => void;
    const resultPromise = reconcileAssistantRequest({
      readStatus: async (signal) => {
        expect(signal).toBe(controller.signal);
        return new Promise<AssistantRequestStatus>((resolve) => {
          resolveRead = resolve;
        });
      },
      signal: controller.signal,
      wait: noWait,
    });

    controller.abort();
    resolveRead(status('completed'));

    await expect(resultPromise).resolves.toEqual({ kind: 'aborted' });
  });
});
