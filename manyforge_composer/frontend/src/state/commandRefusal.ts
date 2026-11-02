import { ERROR_CATALOG } from '../api/errorCatalog.generated';
import { parseApiErrorDto } from '../api/parsers/programRuntime';
import type { StructuredError } from '../types/api';
import { operatorAlarmRefusalMessage } from './operatorAlarmRefusal';
import { formatStructuredError } from './structuredError';

/** The error catalog's `[compact, full]` text for a code, or null without a row. */
export function errorCatalogRow(code: string): readonly [string, string] | null {
  return Object.hasOwn(ERROR_CATALOG, code) ? ERROR_CATALOG[code] : null;
}

export interface ErrorPresentation {
  /** The catalog's compact text; the producer's text for a code without a row. */
  text: string;
  /** The catalog's full text, then the producer's text; null without a row. */
  detail: string | null;
}

/**
 * How one typed error is shown: the catalog's compact text, with its full text
 * and the producer's message (and bounded details) as detail. A code without a
 * catalog row falls back to the producer's message. Display only.
 */
export function errorPresentation(error: StructuredError): ErrorPresentation {
  const { rows } = formatStructuredError(error);
  const lead = error.message || error.code;
  const producerText = rows.length === 0
    ? lead
    : `${lead} (${rows.map((row) => `${row.label}: ${row.value}`).join('; ')})`;
  const row = errorCatalogRow(error.code);
  if (row === null) return { text: producerText, detail: null };
  const [compact, full] = row;
  return { text: compact, detail: full === '' ? producerText : `${full} ${producerText}` };
}

/** CONTRACTS §8: a global rule holds its scope (details `rule_id`). */
export const RULE_HOLDS = 'rule_holds';

/** The operator text of a `rule_holds` refusal, naming the rule when known. */
export function ruleHoldsMessage(ruleId: string | null): string {
  const rule = ruleId === null ? 'A global interrupt rule' : `Global interrupt rule ${ruleId}`;
  return `${rule} holds this scope: its condition still holds after its Stop, or it can't watch its signals. It clears by itself.`;
}

/** Classify the owner's typed code, never the rendered message. */
export function isRuleHoldsRefusal(error: StructuredError | null | undefined): boolean {
  return error?.code === RULE_HOLDS;
}

/**
 * The one operator text for an execution-command refusal, whichever path it
 * arrived on (HTTP error, REST receipt or WebSocket receipt). Display only: it
 * never changes the disposition or treats a refusal as no-effect.
 *
 * The specific guidance first (alarm attention, a holding rule), otherwise the
 * catalog presentation on one line (`compact: full producer message`), e.g.
 * `Too many retained requests: … cell-admission request rejected (Reason: claim_capacity_exhausted)`.
 * `withCode` keeps the receipt paths' `code: ` lead.
 */
export function commandRefusalText(
  error: StructuredError,
  { withCode = false }: { withCode?: boolean } = {},
): string {
  const ruleId = error.details?.rule_id;
  const guidance = operatorAlarmRefusalMessage(error.code)
    ?? (isRuleHoldsRefusal(error) ? ruleHoldsMessage(typeof ruleId === 'string' && ruleId !== '' ? ruleId : null) : null);
  if (guidance !== null) return guidance;
  const { text, detail } = errorPresentation(error);
  const line = detail === null ? text : `${text}: ${detail}`;
  return withCode ? `${error.code}: ${line}` : line;
}

/** Keep the owner's typed refusal with its already-rendered text. */
export class CommandRefusalError extends Error {
  readonly structuredError: StructuredError;
  readonly executionResult?: unknown;

  constructor(structuredError: StructuredError, options?: { withCode?: boolean; executionResult?: unknown }) {
    super(commandRefusalText(structuredError, options));
    this.structuredError = structuredError;
    this.executionResult = options?.executionResult;
    this.name = 'CommandRefusalError';
  }
}

/** A command or API error may carry an owner-issued ErrorDTO. */
export function commandRefusalCause(error: unknown): StructuredError | null {
  if (!(error instanceof Error)) return null;
  const cause = (error as Error & { structuredError?: StructuredError | null }).structuredError;
  return cause !== null && cause !== undefined && typeof cause.code === 'string' ? cause : null;
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
  const parsed = parseApiErrorDto({
    detail: { code, message, details: owner?.details ?? null, producer: owner?.producer },
  });
  return parsed ?? { code, message, details: null, origin: null };
}
