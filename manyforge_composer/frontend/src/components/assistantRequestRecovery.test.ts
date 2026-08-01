import { describe, expect, it } from 'vitest';
import {
  reconcileAssistantRequest,
  shouldReconcileAssistantRequest,
} from './assistantRequestRecovery';

interface Status {
  status: 'accepted' | 'running' | 'completed' | 'failed';
  detail: string;
}

function status(state: Status['status']): Status {
  return { status: state, detail: state };
}

const isTerminal = (value: Status) =>
  value.status === 'completed' || value.status === 'failed';
const noWait = async () => undefined;

describe('authoritative request reconciliation', () => {
  it('reconciles transport/server failures but not definitive client errors', () => {
    expect(shouldReconcileAssistantRequest(new TypeError('network error'))).toBe(true);
    expect(shouldReconcileAssistantRequest({ status: 500 })).toBe(true);
    expect(shouldReconcileAssistantRequest({ status: 422 })).toBe(false);
  });

  it('returns the complete authoritative terminal record', async () => {
    const responses = [status('running'), status('completed')];
    await expect(
      reconcileAssistantRequest({
        readStatus: async () => responses.shift() ?? status('completed'),
        isTerminal,
        wait: noWait,
      }),
    ).resolves.toEqual({ kind: 'terminal', status: status('completed') });
  });

  it('bounds initial 404 races and remembers an observed request', async () => {
    let attempt = 0;
    const result = await reconcileAssistantRequest({
      readStatus: async () => {
        attempt += 1;
        if (attempt === 1) return status('running');
        throw { status: 404 };
      },
      isTerminal,
      maxNotFoundAttempts: 2,
      wait: noWait,
    });
    expect(result).toEqual({ kind: 'not_found', requestObserved: true });
  });

  it('survives initial not-found races until the request becomes terminal', async () => {
    let attempt = 0;
    await expect(reconcileAssistantRequest({
      readStatus: async () => {
        attempt += 1;
        if (attempt < 3) throw { status: 404 };
        return status('completed');
      },
      isTerminal,
      maxNotFoundAttempts: 3,
      wait: noWait,
    })).resolves.toEqual({ kind: 'terminal', status: status('completed') });
    expect(attempt).toBe(3);
  });

  it('reports when repeated not-found responses never observed the request', async () => {
    await expect(reconcileAssistantRequest({
      readStatus: async () => { throw { status: 404 }; },
      isTerminal,
      maxNotFoundAttempts: 2,
      wait: noWait,
    })).resolves.toEqual({ kind: 'not_found', requestObserved: false });
  });

  it('bounds repeated read failures and non-terminal polling', async () => {
    await expect(
      reconcileAssistantRequest({
        readStatus: async () => {
          throw new TypeError('network error');
        },
        isTerminal,
        maxReadFailures: 2,
        wait: noWait,
      }),
    ).resolves.toEqual({ kind: 'unreachable', detail: 'network error' });
    await expect(
      reconcileAssistantRequest({
        readStatus: async () => status('running'),
        isTerminal,
        maxPollAttempts: 2,
        wait: noWait,
      }),
    ).resolves.toEqual({ kind: 'exhausted' });
  });

  it('recovers from transient status-read errors before a terminal response', async () => {
    let attempt = 0;
    await expect(reconcileAssistantRequest({
      readStatus: async () => {
        attempt += 1;
        if (attempt < 3) throw new TypeError('temporary transport failure');
        return status('failed');
      },
      isTerminal,
      maxReadFailures: 3,
      wait: noWait,
    })).resolves.toEqual({ kind: 'terminal', status: status('failed') });
  });

  it('does not read status when reconciliation is already aborted', async () => {
    const controller = new AbortController();
    controller.abort();
    let reads = 0;
    await expect(reconcileAssistantRequest({
      readStatus: async () => {
        reads += 1;
        return status('completed');
      },
      isTerminal,
      signal: controller.signal,
      wait: noWait,
    })).resolves.toEqual({ kind: 'aborted' });
    expect(reads).toBe(0);
  });

  it('lets abort win over a terminal response already being read', async () => {
    const controller = new AbortController();
    let resolveRead!: (value: Status) => void;
    const result = reconcileAssistantRequest({
      readStatus: async () => new Promise<Status>((resolve) => {
        resolveRead = resolve;
      }),
      isTerminal,
      signal: controller.signal,
      wait: noWait,
    });
    controller.abort();
    resolveRead(status('completed'));
    await expect(result).resolves.toEqual({ kind: 'aborted' });
  });
});
