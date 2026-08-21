import type {
  ManagerApplyPendingResponse,
  ManagerApplyRequest,
  ManagerApplyResponse,
  ManagerApplySucceededResponse,
} from '../types/api';

export const MANAGER_APPLY_OBSERVATION_BOUND_MS = 15_000;
export const MANAGER_APPLY_REPLAY_INTERVAL_MS = 1_000;
const MAX_MANAGER_APPLY_REPLAYS = 64;

type ManagerApplyCall = (
  request: ManagerApplyRequest,
  signal?: AbortSignal,
) => Promise<ManagerApplyResponse>;

type DurableManagerApplyIdentity = Pick<
  ManagerApplyResponse,
  'applyId' | 'applyGeneration'
>;

export interface ManagerApplyCompletionOptions {
  signal?: AbortSignal;
  overallTimeoutMs?: number;
  replayIntervalMs?: number;
  now?: () => number;
  sleep?: (delayMs: number, signal?: AbortSignal) => Promise<void>;
}

export type ManagerApplyCompletion =
  | {
      state: 'succeeded';
      response: ManagerApplySucceededResponse;
    }
  | {
      state: 'pending_timeout';
      response: ManagerApplyPendingResponse;
    };

function boundedDuration(value: number, label: string): number {
  if (!Number.isFinite(value) || value <= 0) {
    throw new RangeError(`${label} must be a positive finite duration`);
  }
  return value;
}

function delay(delayMs: number, signal?: AbortSignal): Promise<void> {
  return new Promise((resolve, reject) => {
    function abort() {
      globalThis.clearTimeout(timer);
      signal?.removeEventListener('abort', abort);
      reject(signal?.reason ?? new DOMException('Aborted', 'AbortError'));
    }
    const timer = globalThis.setTimeout(() => {
      signal?.removeEventListener('abort', abort);
      resolve();
    }, delayMs);
    signal?.addEventListener('abort', abort, { once: true });
    if (signal?.aborted) abort();
  });
}

/**
 * Observe one durable Manager Apply by replaying the exact original request.
 * A pending receipt is never converted into success; it is returned as a
 * bounded timeout result when the observation window expires.
 */
export async function completeManagerApply(
  request: ManagerApplyRequest,
  apply: ManagerApplyCall,
  options: ManagerApplyCompletionOptions = {},
): Promise<ManagerApplyCompletion> {
  const overallTimeoutMs = boundedDuration(
    options.overallTimeoutMs ?? MANAGER_APPLY_OBSERVATION_BOUND_MS,
    'overallTimeoutMs',
  );
  const replayIntervalMs = boundedDuration(
    options.replayIntervalMs ?? MANAGER_APPLY_REPLAY_INTERVAL_MS,
    'replayIntervalMs',
  );
  const now = options.now ?? Date.now;
  const sleep = options.sleep ?? delay;
  const startedAt = now();
  const deadline = startedAt + overallTimeoutMs;
  const maximumReplays = Math.min(
    MAX_MANAGER_APPLY_REPLAYS,
    Math.max(1, Math.ceil(overallTimeoutMs / replayIntervalMs)),
  );
  const operationController = new AbortController();
  let timedOut = false;
  const externalAbort = () => {
    operationController.abort(options.signal?.reason);
  };
  if (options.signal?.aborted) {
    externalAbort();
  } else {
    options.signal?.addEventListener('abort', externalAbort, { once: true });
  }
  const timeout = globalThis.setTimeout(() => {
    timedOut = true;
    operationController.abort(new DOMException('Timed out', 'TimeoutError'));
  }, overallTimeoutMs);
  let response: ManagerApplyResponse;
  let pendingResponse: ManagerApplyPendingResponse | undefined;
  let durableIdentity: DurableManagerApplyIdentity | undefined;
  let identityMismatch = false;
  const observeDurableIdentity = (receipt: ManagerApplyResponse) => {
    if (durableIdentity === undefined) {
      durableIdentity = {
        applyId: receipt.applyId,
        applyGeneration: receipt.applyGeneration,
      };
      return;
    }
    if (
      receipt.applyId !== durableIdentity.applyId
      || receipt.applyGeneration !== durableIdentity.applyGeneration
    ) {
      identityMismatch = true;
      throw new Error(
        'Manager Apply replay changed the durable Apply identity or generation',
      );
    }
  };
  try {
    response = await apply(request, operationController.signal);
    observeDurableIdentity(response);
    if (response.state === 'succeeded') {
      return { state: 'succeeded', response };
    }
    pendingResponse = response;

    for (let replay = 0; replay < maximumReplays; replay += 1) {
      const remainingMs = deadline - now();
      if (remainingMs <= 0) break;
      await sleep(
        Math.min(replayIntervalMs, remainingMs),
        operationController.signal,
      );
      if (deadline - now() <= 0) break;
      response = await apply(request, operationController.signal);
      observeDurableIdentity(response);
      if (response.state === 'succeeded') {
        return { state: 'succeeded', response };
      }
      pendingResponse = response;
    }

    if (pendingResponse === undefined) {
      throw new Error('Manager Apply observation lost its pending receipt');
    }
    return { state: 'pending_timeout', response: pendingResponse };
  } catch (error) {
    if (timedOut && pendingResponse !== undefined && !identityMismatch) {
      return { state: 'pending_timeout', response: pendingResponse };
    }
    throw error;
  } finally {
    globalThis.clearTimeout(timeout);
    options.signal?.removeEventListener('abort', externalAbort);
  }
}
