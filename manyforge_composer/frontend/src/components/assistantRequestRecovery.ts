import type { AssistantRequestStatus } from '../types/api';

const TERMINAL_ASSISTANT_STATUSES = new Set<AssistantRequestStatus['status']>([
  'completed',
  'failed',
  'cancelled',
]);

export type AssistantRequestReconciliationResult =
  | {
      kind: 'terminal';
      status: AssistantRequestStatus;
    }
  | {
      kind: 'not_found';
      requestObserved: boolean;
    }
  | {
      kind: 'unreachable';
      detail: string;
    }
  | {
      kind: 'exhausted';
    }
  | {
      kind: 'aborted';
    };

export interface AssistantRequestReconciliationOptions {
  readStatus: (signal?: AbortSignal) => Promise<AssistantRequestStatus>;
  signal?: AbortSignal;
  retryDelayMs?: number;
  maxNotFoundAttempts?: number;
  maxReadFailures?: number;
  maxPollAttempts?: number;
  wait?: (delayMs: number, signal?: AbortSignal) => Promise<void>;
}

function errorStatus(error: unknown): number | null {
  if (!error || typeof error !== 'object' || !('status' in error)) {
    return null;
  }
  const status = (error as { status?: unknown }).status;
  return typeof status === 'number' ? status : null;
}

function errorDetail(error: unknown): string {
  return error instanceof Error
    ? error.message
    : 'The assistant status endpoint could not be reached.';
}

function waitForRetry(delayMs: number, signal?: AbortSignal): Promise<void> {
  return new Promise((resolve) => {
    if (signal?.aborted) {
      resolve();
      return;
    }

    let settled = false;
    const finish = () => {
      if (settled) {
        return;
      }
      settled = true;
      signal?.removeEventListener('abort', onAbort);
      resolve();
    };
    const onAbort = () => {
      globalThis.clearTimeout(timeoutId);
      finish();
    };
    const timeoutId = globalThis.setTimeout(finish, delayMs);
    signal?.addEventListener('abort', onAbort, { once: true });
  });
}

export function shouldReconcileAssistantRequest(error: unknown): boolean {
  const status = errorStatus(error);
  return status === null || status >= 500;
}

export async function reconcileAssistantRequest(
  options: AssistantRequestReconciliationOptions,
): Promise<AssistantRequestReconciliationResult> {
  const retryDelayMs = options.retryDelayMs ?? 2_000;
  const maxNotFoundAttempts = options.maxNotFoundAttempts ?? 5;
  const maxReadFailures = options.maxReadFailures ?? 5;
  const maxPollAttempts = Math.max(1, options.maxPollAttempts ?? 150);
  const wait = options.wait ?? waitForRetry;
  let notFoundAttempts = 0;
  let readFailures = 0;
  let pollAttempts = 0;
  let requestObserved = false;

  while (!options.signal?.aborted) {
    pollAttempts += 1;
    try {
      const status = await options.readStatus(options.signal);
      if (options.signal?.aborted) {
        return { kind: 'aborted' };
      }
      requestObserved = true;
      notFoundAttempts = 0;
      readFailures = 0;
      if (TERMINAL_ASSISTANT_STATUSES.has(status.status)) {
        return { kind: 'terminal', status };
      }
    } catch (error) {
      if (options.signal?.aborted) {
        return { kind: 'aborted' };
      }
      if (errorStatus(error) === 404) {
        notFoundAttempts += 1;
        if (notFoundAttempts >= maxNotFoundAttempts) {
          return { kind: 'not_found', requestObserved };
        }
      } else {
        readFailures += 1;
        if (readFailures >= maxReadFailures) {
          return { kind: 'unreachable', detail: errorDetail(error) };
        }
      }
    }

    if (pollAttempts >= maxPollAttempts) {
      return { kind: 'exhausted' };
    }
    await wait(retryDelayMs, options.signal);
  }

  return { kind: 'aborted' };
}
