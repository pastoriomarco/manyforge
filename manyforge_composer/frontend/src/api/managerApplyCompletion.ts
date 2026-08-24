import type {
  ManagerApplyPendingResponse,
  ManagerApplyRequest,
  ManagerApplyResponse,
  ManagerApplyStatusResponse,
  ManagerApplySucceededResponse,
} from '../types/api';

export const MANAGER_APPLY_OBSERVATION_BOUND_MS = 15_000;
export const MANAGER_APPLY_OBSERVATION_INTERVAL_MS = 1_000;
const MAX_MANAGER_APPLY_OBSERVATIONS = 64;

type ManagerApplySubmit = (
  request: ManagerApplyRequest,
  signal?: AbortSignal,
) => Promise<ManagerApplyResponse>;
type ManagerApplyObserve = (
  applyId: string,
  applyGeneration: number,
  signal?: AbortSignal,
) => Promise<ManagerApplyStatusResponse>;

export interface ManagerApplyCompletionOptions {
  signal?: AbortSignal;
  overallTimeoutMs?: number;
  observationIntervalMs?: number;
  now?: () => number;
  sleep?: (delayMs: number, signal?: AbortSignal) => Promise<void>;
}

export type ManagerApplyCompletion =
  | { state: 'succeeded'; response: ManagerApplySucceededResponse }
  | { state: 'pending_timeout'; response: ManagerApplyPendingResponse };

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

/** Resume observation from a previously retained durable submission receipt. */
export async function observePendingManagerApply(
  receipt: ManagerApplyPendingResponse,
  observe: ManagerApplyObserve,
  options: ManagerApplyCompletionOptions = {},
): Promise<ManagerApplyCompletion> {
  if (options.signal?.aborted) throw options.signal.reason;
  const overallTimeoutMs = boundedDuration(
    options.overallTimeoutMs ?? MANAGER_APPLY_OBSERVATION_BOUND_MS,
    'overallTimeoutMs',
  );
  const intervalMs = boundedDuration(
    options.observationIntervalMs ?? MANAGER_APPLY_OBSERVATION_INTERVAL_MS,
    'observationIntervalMs',
  );
  const now = options.now ?? Date.now;
  const sleep = options.sleep ?? delay;
  const deadline = now() + overallTimeoutMs;
  let pending = receipt;
  for (let count = 0; count < MAX_MANAGER_APPLY_OBSERVATIONS; count += 1) {
    const remaining = deadline - now();
    if (remaining <= 0) break;
    await sleep(Math.min(intervalMs, remaining), options.signal);
    if (deadline - now() <= 0) break;
    const status = await observe(
      receipt.applyId,
      receipt.applyGeneration,
      options.signal,
    );
    if (status.state === 'succeeded') {
      return {
        state: 'succeeded',
        response: {
          ...status,
          replayStatus: receipt.replayStatus,
          requestId: receipt.requestId,
          requestOwnerEpoch: receipt.requestOwnerEpoch,
        },
      };
    }
    pending = {
      ...status,
      replayStatus: receipt.replayStatus,
      requestId: receipt.requestId,
      requestOwnerEpoch: receipt.requestOwnerEpoch,
    };
  }
  return { state: 'pending_timeout', response: pending };
}

/** Submit once, then use only the read-only durable-operation status. */
export async function completeManagerApply(
  request: ManagerApplyRequest,
  submit: ManagerApplySubmit,
  observe: ManagerApplyObserve,
  options: ManagerApplyCompletionOptions = {},
): Promise<ManagerApplyCompletion> {
  if (options.signal?.aborted) throw options.signal.reason;
  const receipt = await submit(request, options.signal);
  if (receipt.state === 'succeeded') {
    return { state: 'succeeded', response: receipt };
  }
  return observePendingManagerApply(receipt, observe, options);
}
