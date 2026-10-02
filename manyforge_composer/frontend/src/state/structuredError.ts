import type { StructuredError } from '../types/api';

/**
 * The one display projection of a StructuredError: run status, recovery offer
 * and API refusals all render their failure through these functions.
 *
 * Display only. Nothing here selects, enables or orders a recovery action;
 * those come from the owner's offer and run state. The message is shown as the
 * producer wrote it and is never parsed or rebuilt from the details.
 */

export interface StructuredErrorRow {
  label: string;
  value: string;
}

export interface StructuredErrorDisplay {
  /** The humanized stable code. */
  title: string;
  /** The producer's message; empty when the carrier has none. */
  message: string;
  /** Labelled details, then the origin. */
  rows: StructuredErrorRow[];
}

type FailureContext = Pick<StructuredError, 'details' | 'origin'>;

/** A stable code as words: `owner_refused` → `owner refused`. */
export function humanizeReasonCode(code: string): string {
  return code.trim().replaceAll('_', ' ');
}

function quoted(value: string): string {
  return `'${value}'`;
}

const DETAIL_LABELS: Readonly<Record<string, string>> = {
  latch_id: 'Latch',
};

function detailValue(value: string | number | boolean | null): string {
  if (value === null) return 'none';
  if (typeof value === 'boolean') return value ? 'yes' : 'no';
  return String(value);
}

/** Labelled rows for a failure's bounded details and origin. */
export function structuredErrorRows({ details, origin }: FailureContext): StructuredErrorRow[] {
  const rows: StructuredErrorRow[] = [];
  const shown = new Set<string>();
  const text = (key: string): string | null => {
    const value = details?.[key];
    if (typeof value !== 'string' || value.length === 0) return null;
    shown.add(key);
    return value;
  };
  const reason = text('reason');
  if (reason !== null) rows.push({ label: 'Reason', value: reason });
  const referenceType = text('reference_type');
  if (referenceType !== null) {
    const referenceId = text('reference_id');
    const targetRoleId = text('target_role_id');
    rows.push({
      label: 'Reference',
      value: `${referenceType}${referenceId === null ? '' : ` ${quoted(referenceId)}`}${
        targetRoleId === null ? '' : ` of target role ${quoted(targetRoleId)}`}`,
    });
  }
  const sourceKind = text('source_kind');
  if (sourceKind !== null) {
    const sourceReference = text('source_reference');
    const problem = text('input_problem');
    rows.push({
      label: 'Input',
      value: `${text('path') ?? 'node inputs'} from ${humanizeReasonCode(sourceKind)}${
        sourceReference === null ? '' : ` ${quoted(sourceReference)}`}${
        problem === null ? '' : ` (${problem})`}`,
    });
    // A null path or reference is part of this row, not a separate detail.
    shown.add('path').add('source_reference').add('input_problem');
  }
  const requiredAction = text('required_action');
  if (requiredAction !== null) {
    rows.push({ label: 'Required action', value: humanizeReasonCode(requiredAction) });
  }
  // Every other detail (an API refusal's details are the code's own payload)
  // is shown under its own key.
  for (const key of Object.keys(details ?? {}).sort()) {
    if (shown.has(key)) continue;
    const label = DETAIL_LABELS[key] ?? humanizeReasonCode(key);
    rows.push({
      label: label.charAt(0).toUpperCase() + label.slice(1),
      value: detailValue(details![key]),
    });
  }
  if (origin !== null) {
    const node = origin.nodeInstanceId === null ? null : `node ${quoted(origin.nodeInstanceId)}`;
    const value = origin.kind === 'program_node'
      ? node!
      : origin.kind === 'owner'
        ? `${origin.ownerApi ?? 'owner'} for ${node}`
        : `Program runner${node === null ? '' : ` at ${node}`}`;
    rows.push({ label: 'Origin', value });
  }
  return rows;
}

export function formatStructuredError(error: StructuredError): StructuredErrorDisplay {
  return {
    title: humanizeReasonCode(error.code),
    message: error.message,
    rows: structuredErrorRows(error),
  };
}

function rowsText(rows: readonly StructuredErrorRow[]): string {
  return rows.map((row) => `${row.label}: ${row.value}`).join('; ');
}

/** The rows on one line (`Reference: …; Origin: …`), or null when there are none. */
export function failureContextText(context: FailureContext): string | null {
  const rows = structuredErrorRows(context);
  return rows.length === 0 ? null : rowsText(rows);
}

/** One line for notices and summaries: the message (or the code) and its rows. */
export function structuredErrorText(error: StructuredError): string {
  const { title, message, rows } = formatStructuredError(error);
  const lead = message || title;
  return rows.length === 0 ? lead : `${lead} (${rowsText(rows)})`;
}
