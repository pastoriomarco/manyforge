/**
 * Opens the Deployment's global interrupt rules from another surface (the
 * Program node editor). The App switches to the Deployment workspace; the
 * rules section, once mounted, takes the pending request and reveals itself.
 */
const OPEN_GLOBAL_RULES_EVENT = 'manyforge:open-global-rules';

let pending = false;

export function requestOpenGlobalRules(): void {
  pending = true;
  globalThis.dispatchEvent?.(new CustomEvent(OPEN_GLOBAL_RULES_EVENT));
}

/** Subscribe; returns the unsubscribe function (for a `useEffect`). */
export function onOpenGlobalRulesRequest(open: () => void): () => void {
  globalThis.addEventListener?.(OPEN_GLOBAL_RULES_EVENT, open);
  return () => globalThis.removeEventListener?.(OPEN_GLOBAL_RULES_EVENT, open);
}

/** Whether a request is pending; taking it clears it. */
export function takeOpenGlobalRulesRequest(): boolean {
  const requested = pending;
  pending = false;
  return requested;
}
