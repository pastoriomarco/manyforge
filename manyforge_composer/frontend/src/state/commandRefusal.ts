import { parseApiErrorDto } from '../api/parsers/programRuntime';
import type { StructuredError } from '../types/api';
import { motionWaitRefusalMessage } from './motionWaitRefusal';
import { operatorAlarmRefusalMessage } from './operatorAlarmRefusal';
import { formatStructuredError } from './structuredError';

/**
 * The one operator text for an execution-command refusal, whichever path it
 * arrived on (HTTP error, REST receipt or WebSocket receipt). Display only: it
 * never changes the disposition or treats a refusal as no-effect.
 *
 * The specific guidance first (alarm attention, motion wait), otherwise the
 * owner's message (or code) followed by its bounded details, e.g.
 * `cell-admission request rejected (Reason: expected_generation_stale)`.
 * `withCode` keeps the receipt paths' `code: ` lead.
 */
export function commandRefusalText(
  error: StructuredError,
  { withCode = false }: { withCode?: boolean } = {},
): string {
  const guidance = operatorAlarmRefusalMessage(error.code, error.details)
    ?? motionWaitRefusalMessage(error.code, error.details);
  if (guidance !== null) return guidance;
  const { rows } = formatStructuredError(error);
  const lead = error.message || error.code;
  const text = rows.length === 0
    ? lead
    : `${lead} (${rows.map((row) => `${row.label}: ${row.value}`).join('; ')})`;
  return withCode ? `${error.code}: ${text}` : text;
}

/**
 * The owner's refusal carried by a rejected receipt's result: the result
 * itself or its `error` member, under the receipt code.
 */
export function receiptRefusal(code: string, result: unknown): StructuredError {
  const record = result !== null && typeof result === 'object' && !Array.isArray(result)
    ? result as Record<string, unknown> : null;
  const owner = record?.error !== null && typeof record?.error === 'object'
    && !Array.isArray(record.error) ? record.error as Record<string, unknown> : record;
  const message = typeof record?.message === 'string' ? record.message
    : typeof owner?.message === 'string' ? owner.message : '';
  const parsed = parseApiErrorDto({ detail: { code, message, details: owner?.details ?? null } });
  return parsed ?? { code, message, details: null, origin: null };
}
