import { humanizeReasonCode } from '../state/structuredError';

export interface KernelErrorView {
  code: string;
  message: string;
  details?: Record<string, unknown> | null;
}

export interface KernelErrorPresentation {
  /** The cause: what went wrong. */
  headline: string;
  /** Category, scope and lifecycle under the headline, joined with " · ". */
  status?: string;
  /** What to do; omitted when empty. */
  hint: string;
  severity: 'warning' | 'error' | 'critical';
}

function stableValue(value: unknown): unknown {
  if (Array.isArray(value)) return value.map(stableValue);
  if (value !== null && typeof value === 'object') {
    return Object.fromEntries(
      Object.entries(value as Record<string, unknown>)
        .sort(([left], [right]) => left.localeCompare(right))
        .map(([key, nested]) => [key, stableValue(nested)]),
    );
  }
  return value;
}

const ERROR_PRESENTATION_IDENTITY_KEYS = [
  'runtime_fault_id',
  'execution_id',
  'action_span_id',
  'node_attempt_id',
] as const;

/**
 * Keep one incident stable across incremental diagnostic enrichment. Retained
 * from source blob aaa1d7fd40e2692773cf46b2c9549bb639104d61 and test blob
 * fc9a9d1ad41db46dc8d9aafe6e2593977652a4ce; interpreting the diagnostic code
 * and choosing remediation copy remain an I8 projection job.
 */
export function errorPresentationKey(error: KernelErrorView): string {
  const identities = ERROR_PRESENTATION_IDENTITY_KEYS.flatMap((key) => {
    const value = error.details?.[key];
    return value === null || value === undefined || value === ''
      ? []
      : [`${key}=${String(value)}`];
  });
  if (identities.length > 0) {
    return `${error.code}|${identities.join('|')}`;
  }
  return JSON.stringify(stableValue({
    code: error.code,
    message: error.message,
    details: error.details ?? null,
  }));
}

export function readableErrorCode(code: string): string {
  return humanizeReasonCode(code) || 'Runtime error';
}

export function prettyBodyId(bodyId: string): string {
  const segments = bodyId.split('/').filter(Boolean);
  if (segments[0] === 'object' && segments.length >= 2) {
    return segments[1];
  }
  if (
    segments[0] === 'robot'
    && segments.length >= 4
    && (segments[2] === 'link' || segments[2] === 'attached')
  ) {
    return segments[3];
  }
  return segments[segments.length - 1] || bodyId;
}

export function prettyPair(pair: string): string {
  const parts = pair.split(/\s*<->\s*/);
  if (parts.length !== 2) return pair;
  const short = (token: string): string => {
    const segments = token.split('/').filter(Boolean);
    if (segments[0] === 'object' && segments.length >= 2) {
      return segments[1];
    }
    if (segments[0] === 'robot' && segments.length >= 4) {
      if (segments[2] === 'link') return segments[3];
      if (segments[2] === 'attached') return `attached ${segments[3]}`;
    }
    return segments[segments.length - 1] || token;
  };
  return `${short(parts[0])} ↔ ${short(parts[1])}`;
}

function detailRows(details: Record<string, unknown> | null | undefined) {
  return Object.entries(details ?? {}).map(([key, value]) => ({
    key,
    value: typeof value === 'string' ? value : JSON.stringify(value),
  }));
}

interface KernelErrorPanelProps {
  error: KernelErrorView;
  presentation?: KernelErrorPresentation;
  presentationKey?: string;
  onDismiss?: () => void;
  eyebrow?: string;
}

/**
 * Retained diagnostic presentation shell. I8 owns the exact Kernel projection,
 * code taxonomy, Stop-chain interpretation, and remediation text; this client
 * does not preserve the deleted browser-side error-policy table.
 */
export function KernelErrorPanel({
  error,
  presentation,
  presentationKey,
  onDismiss,
  eyebrow = 'Kernel diagnostic',
}: KernelErrorPanelProps) {
  const rows = detailRows(error.details);
  const headline = presentation?.headline || readableErrorCode(error.code);
  const hint = presentation === undefined ? error.message : presentation.hint;
  const severity = presentation?.severity ?? 'error';
  return (
    <section
      className={`kernel-error-panel severity-${severity}`}
      role="alert"
      data-presentation-key={presentationKey ?? errorPresentationKey(error)}
    >
      <header>
        <div>
          <p className="eyebrow">{eyebrow}</p>
          <h2>{headline}</h2>
          {presentation?.status && <p className="kernel-error-status">{presentation.status}</p>}
        </div>
        {onDismiss && (
          <button type="button" onClick={onDismiss}>Dismiss</button>
        )}
      </header>
      {hint && <p>{hint}</p>}
      <details className="kernel-error-technical">
        <summary>Technical details</summary>
        <code>{error.code || 'unclassified'}</code>
        {rows.length > 0 && (
          <dl>
            {rows.map((row) => (
              <div key={row.key}>
                <dt>{row.key.replaceAll('_', ' ')}</dt>
                <dd>{row.value}</dd>
              </div>
            ))}
          </dl>
        )}
      </details>
    </section>
  );
}
