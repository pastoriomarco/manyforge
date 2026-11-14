import { commandRowWithKey } from '../api/parsers/commandRows';
import type {
  ApplyCommandRow,
  ApplyCommandRows,
  CommandRow,
  CommandRows,
  FailureDetails,
  StructuredError,
} from '../types/api';
import type { CodedError } from '../types/components';
import type { BusyOperation } from './busyOperation';
import { errorCatalogRow } from './commandRefusal';

/**
 * What the operator reads from a command row (OD-214): which command, the
 * step it is in, and on an end other than success the error catalogue's text
 * for its typed code. The row carries no message text.
 */

// GUI labels: proposals, to be finalised by the coordinator.
const COMMAND_KIND_LABELS: Readonly<Record<string, string>> = {
  start: 'Execute',
  reset: 'Reset',
  scene_preparation: 'Scene preparation',
  runtime_blackboard_write: 'Program value write',
  manual_result_stage: 'Manual result',
  recovery_action: 'Recovery action',
  resume_halt: 'Execute after Halt',
  start_motion: 'Run',
  resume_motion: 'Resume',
  prepare_manual_handover: 'Manual handover',
  persistent_value_set: 'Set stored value',
  persistent_value_reset: 'Reset stored value',
  persistent_values_clear: 'Clear stored values',
  deployment_apply: 'Apply',
  deployment_revert: 'Revert',
  scenario_load: 'Load',
  scenario_new: 'New Scenario',
  program_apply: 'Program Apply',
  program_revert: 'Program Revert',
  activation_preparation: 'Preparation',
};

const COMMAND_STEP_LABELS: Readonly<Record<string, string>> = {
  admitting: 'checking',
  claiming: 'claiming the cell',
  starting_run: 'starting the run',
  applying: 'applying',
  recovering: 'recovering',
  resuming: 'resuming',
  stopping: 'stopping',
  settling: 'waiting for motion to settle',
  restoring_scene: 'restoring the Scene',
  restoring_position: 'moving the robot back',
  releasing: 'releasing the cell',
  preparing_scene: 'preparing the Scene',
  emptying: 'emptying the cell',
  resetting_cycle: 'resetting the Program',
  previewing: 'preparing the configuration',
  disabling: 'disabling components',
  clearing_scene: 'clearing the Scene',
  preparing_components: 'preparing components',
  awaiting_admission: 'waiting for the cell',
  preparing: 'preparing',
};

const BEING_ADMITTED = 'starting';
const NO_CATALOGUE_TEXT = 'Ended without a known reason.';

export function commandKindLabel(kind: string): string {
  return COMMAND_KIND_LABELS[kind] ?? 'Command';
}

/** The step a row is in, as words; a row with no step yet is starting. */
export function commandStepText(row: CommandRow): string {
  return row.step === null ? BEING_ADMITTED : COMMAND_STEP_LABELS[row.step] ?? BEING_ADMITTED;
}

/** "Execute, claiming the cell". */
export function commandProgressText(row: CommandRow): string {
  return `${commandKindLabel(row.kind)}, ${commandStepText(row)}`;
}

const APPLY_LEVEL_OPERATIONS = new Set(['apply', 'revert', 'load', 'new']);

/**
 * This page's Apply, Revert, Load or New with the step of the Apply-level
 * command in progress: one Apply runs at a time, so it is this one's.
 */
export function withApplyStep(
  operation: BusyOperation | null,
  rows: ApplyCommandRows | null | undefined,
): BusyOperation | null {
  const row = rows?.inProgress ?? null;
  if (operation === null || row === null || !APPLY_LEVEL_OPERATIONS.has(operation.command)) return operation;
  return { ...operation, step: commandStepText(row) };
}

/**
 * A refused Apply-level row (no effect) as the typed error its HTTP refusal
 * used to carry, so it is shown where that refusal was; null for any other end.
 */
export function applyRowRefusal(row: ApplyCommandRow): CodedError | null {
  return row.end?.outcome === 'refused' && row.end.code !== null
    ? { code: row.end.code, message: '' } : null;
}

/** The activation preparer's typed cause a refused or failed preparation names, if any. */
export function applyRowPreparationCause(row: ApplyCommandRow): string | null {
  const cause = row.end?.code === 'activation_preparation_failed' ? row.end.details?.cause_code : null;
  return typeof cause === 'string' && cause !== '' ? cause : null;
}

/**
 * The reason a command ended other than succeeded, from the error catalogue
 * for its typed code; null for a command that succeeded or still runs.
 */
export function commandEndText(row: CommandRow): string | null {
  const end = row.end;
  if (end === null || end.outcome === 'succeeded' || end.code === null) return null;
  const catalogue = errorCatalogRow(end.code);
  const reason = catalogue === null ? NO_CATALOGUE_TEXT
    : catalogue[1] === '' ? `${catalogue[0]}.` : `${catalogue[0]}. ${catalogue[1]}`;
  return `${commandKindLabel(row.kind)}: ${reason}`;
}

/** The end's typed code and scalar details, for code-based handling (never shown raw). */
function commandEndError(row: CommandRow): StructuredError {
  const details = row.end?.details ?? null;
  return {
    code: row.end?.code ?? '',
    message: '',
    details: details === null ? null : Object.fromEntries(Object.entries(details).filter(
      (entry): entry is [string, string | number | boolean | null] => entry[1] === null
        || ['string', 'number', 'boolean'].includes(typeof entry[1]),
    )) as FailureDetails,
    origin: null,
  };
}

/** A command that ended other than succeeded: its row and catalogue text. */
export class CommandEndedError extends Error {
  readonly row: CommandRow;
  readonly structuredError: StructuredError;

  constructor(row: CommandRow) {
    super(commandEndText(row) ?? commandKindLabel(row.kind));
    this.name = 'CommandEndedError';
    this.row = row;
    this.structuredError = commandEndError(row);
  }
}

/**
 * Settles each waiting request key when its row shows an end in the rows the
 * page observes. There is no time limit: an accepted command always ends.
 * `abandon` settles every wait with null when the rows can no longer show it
 * (a new application start).
 */
export class CommandRowWaiter<Row extends CommandRow = CommandRow> {
  private latest: CommandRows<string, Row> | null = null;
  private readonly waits = new Map<string, Array<(row: Row | null) => void>>();

  observe(rows: CommandRows<string, Row> | null): void {
    this.latest = rows;
    for (const [key, resolvers] of [...this.waits]) {
      const row = commandRowWithKey(rows, key);
      if (row?.end == null) continue;
      this.waits.delete(key);
      for (const resolve of resolvers) resolve(row);
    }
  }

  /** The row with this key once it ended (at once when it already has). */
  wait(requestKey: string): Promise<Row | null> {
    const row = commandRowWithKey(this.latest, requestKey);
    if (row?.end != null) return Promise.resolve(row);
    return new Promise((resolve) => {
      this.waits.set(requestKey, [...(this.waits.get(requestKey) ?? []), resolve]);
    });
  }

  abandon(): void {
    const waits = [...this.waits.values()];
    this.waits.clear();
    this.latest = null;
    for (const resolvers of waits) for (const resolve of resolvers) resolve(null);
  }
}

/** The row once it ended (at once when the reply already carries its end); null when no longer observable. */
export function awaitCommandEnd<Row extends CommandRow>(
  row: Row,
  waiter: CommandRowWaiter<Row>,
): Promise<Row | null> {
  return row.end !== null ? Promise.resolve(row) : waiter.wait(row.requestKey);
}

/** The row's end once it ended; throws CommandEndedError for any end but succeeded. */
export async function awaitCommandSuccess<Row extends CommandRow>(
  row: Row,
  waiter: CommandRowWaiter<Row>,
): Promise<Row | null> {
  const ended = await awaitCommandEnd(row, waiter);
  if (ended !== null && ended.end?.outcome !== 'succeeded') throw new CommandEndedError(ended);
  return ended;
}
