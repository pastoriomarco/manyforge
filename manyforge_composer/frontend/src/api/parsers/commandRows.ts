import type {
  ActivationPreparationRow,
  ApplyCommandRow,
  ApplyCommandRowKind,
  ApplyCommandRows,
  ApplyStepSource,
  CommandEnd,
  CommandOutcome,
  CommandRow,
  CommandRows,
  RunnerCommandRowKind,
} from '../../types/api';
import type { ApplyResult } from '../../types/components';
import type { Nanoseconds } from '../../types/nanoseconds';
import { isApplicationStartId, isRequestKey } from '../applicationIdentity';
import { parseApplyResultComponents } from './components';
import {
  allowedKeysAt,
  boundedArrayAt,
  digestAt,
  exactKeysAt,
  fail,
  jsonRecordAt,
  monotonicNanosecondsAt,
  nonEmptyStringAt,
  objectAt,
  positiveIntegerAt,
  stringAt,
} from './shared';

/**
 * Command rows (OD-214): a command answers at acceptance and publishes its
 * step and its end. The runner's rows sit in the Program cycle status, the
 * Apply-level rows in Composer's shell and Deployment workspace, the
 * activation preparation in Headless's cycle-status route.
 */

/** Ended rows kept by every publisher. */
export const COMMAND_ROWS_ENDED_MAXIMUM = 8;

const RESET_STEPS = ['stopping', 'settling', 'restoring_scene', 'restoring_position', 'releasing'] as const;

/** Each runner kind's closed list of steps. */
export const RUNNER_COMMAND_STEPS: Readonly<Record<RunnerCommandRowKind, readonly string[]>> = {
  start: ['admitting', 'claiming', 'starting_run'],
  start_motion: ['admitting', 'applying'],
  resume_motion: ['admitting', 'applying'],
  prepare_manual_handover: ['admitting', 'applying'],
  resume_halt: ['admitting', 'applying'],
  manual_result_stage: ['admitting', 'applying'],
  runtime_blackboard_write: ['applying'],
  recovery_action: ['admitting', 'recovering', 'resuming', ...RESET_STEPS],
  persistent_value_set: ['applying'],
  persistent_value_reset: ['applying'],
  persistent_values_clear: ['applying'],
  reset: ['admitting', ...RESET_STEPS],
  scene_preparation: ['admitting', 'preparing_scene'],
};

const APPLY_STEPS = [
  'resetting_cycle', 'previewing', 'disabling', 'applying', 'preparing_components',
] as const;

/** Each Apply-level kind's closed list of steps. */
export const APPLY_COMMAND_STEPS: Readonly<Record<ApplyCommandRowKind, readonly string[]>> = {
  deployment_apply: APPLY_STEPS,
  deployment_revert: APPLY_STEPS,
  program_apply: APPLY_STEPS,
  program_revert: APPLY_STEPS,
  scenario_new: [...APPLY_STEPS, 'clearing_scene'],
  scenario_load: ['emptying', ...APPLY_STEPS, 'clearing_scene'],
};

const ACTIVATION_PREPARATION_STEPS = ['awaiting_admission', 'preparing'] as const;

/** The status an Apply step names as the carrier of its detail. */
const APPLY_STEP_SOURCE: Readonly<Record<string, ApplyStepSource['status'] | null>> = {
  emptying: null,
  resetting_cycle: 'runner_command',
  previewing: 'activation_preparation',
  disabling: 'component_action',
  applying: 'manager_apply',
  clearing_scene: 'scene_clear',
  preparing_components: 'component_action',
};

interface RowSpec<Kind extends string> {
  kinds: Readonly<Record<Kind, readonly string[]>>;
  outcomes: readonly CommandOutcome[];
  extraKeys: readonly string[];
  /** An Apply-level row keeps its component rows in `details` also when it succeeded. */
  detailsWhenSucceeded?: true;
}

/**
 * Wall-clock ns since the epoch, kept exact: the owners send it as decimal
 * text, since a JavaScript number cannot hold it.
 */
function wallNanosecondsAt(value: unknown, path: string): Nanoseconds {
  return monotonicNanosecondsAt(value, path);
}

function commandEndAt(value: unknown, path: string, spec: RowSpec<string>): CommandEnd {
  const { outcomes } = spec;
  const record = objectAt(value, path);
  exactKeysAt(record, ['outcome', 'code', 'details', 'finished_at_ns'], path);
  const outcome = stringAt(record.outcome, `${path}.outcome`) as CommandOutcome;
  if (!outcomes.includes(outcome)) return fail(`${path}.outcome`, outcomes.join(', '));
  const code = record.code === null ? null : nonEmptyStringAt(record.code, `${path}.code`);
  if ((code === null) !== (outcome === 'succeeded')) {
    return fail(`${path}.code`, 'null exactly when the command succeeded');
  }
  const details = record.details === null ? null : jsonRecordAt(record.details, `${path}.details`);
  if (outcome === 'succeeded' && details !== null && spec.detailsWhenSucceeded !== true) {
    return fail(`${path}.details`, 'null when succeeded');
  }
  return {
    outcome,
    code,
    details,
    finishedAtNs: wallNanosecondsAt(record.finished_at_ns, `${path}.finished_at_ns`),
  };
}

function commandRowAt<Kind extends string>(
  value: unknown,
  path: string,
  spec: RowSpec<Kind>,
): { row: CommandRow<Kind>; record: Record<string, unknown> } {
  const record = objectAt(value, path);
  exactKeysAt(record, ['kind', 'request_key', 'step', 'accepted_at_ns', 'end', ...spec.extraKeys], path);
  const kind = stringAt(record.kind, `${path}.kind`);
  if (!Object.hasOwn(spec.kinds, kind)) return fail(`${path}.kind`, 'a known command kind');
  if (!isRequestKey(record.request_key)) return fail(`${path}.request_key`, '32 lowercase hex characters');
  const step = record.step === null ? null : stringAt(record.step, `${path}.step`);
  if (step !== null && !spec.kinds[kind as Kind].includes(step)) {
    return fail(`${path}.step`, `a step of ${kind}`);
  }
  const acceptedAtNs = record.accepted_at_ns === null
    ? null : wallNanosecondsAt(record.accepted_at_ns, `${path}.accepted_at_ns`);
  const end = record.end === null ? null : commandEndAt(record.end, `${path}.end`, spec);
  // Before acceptance a row has no step, and ends only refused (a typed
  // refusal) or unknown (an untyped failure).
  if (acceptedAtNs === null && (step !== null
    || (end !== null && end.outcome !== 'refused' && end.outcome !== 'unknown'))) {
    return fail(path, 'no step and no end other than refused or unknown before acceptance');
  }
  return {
    row: { kind: kind as Kind, requestKey: record.request_key, step, acceptedAtNs, end },
    record,
  };
}

function commandRowsAt<Kind extends string, Row extends CommandRow<Kind>>(
  value: unknown,
  path: string,
  rowAt: (value: unknown, path: string) => Row,
): CommandRows<Kind, Row> {
  const record = objectAt(value, path);
  exactKeysAt(record, ['in_progress', 'ended'], path);
  const inProgress = record.in_progress === null ? null : rowAt(record.in_progress, `${path}.in_progress`);
  if (inProgress !== null && inProgress.end !== null) return fail(`${path}.in_progress.end`, 'null while it runs');
  const ended = boundedArrayAt(record.ended, `${path}.ended`, COMMAND_ROWS_ENDED_MAXIMUM)
    .map((row, index) => {
      const parsed = rowAt(row, `${path}.ended[${index}]`);
      if (parsed.end === null) return fail(`${path}.ended[${index}].end`, 'the end of an ended command');
      return parsed;
    });
  return { inProgress, ended };
}

const RUNNER_ROW: RowSpec<RunnerCommandRowKind> = {
  kinds: RUNNER_COMMAND_STEPS,
  // A Stop never cuts a runner command off: no runner row ends interrupted.
  outcomes: ['succeeded', 'refused', 'failed', 'unknown'],
  extraKeys: [],
};

export function parseRunnerCommandRow(value: unknown, path = 'runnerCommand'): CommandRow<RunnerCommandRowKind> {
  return commandRowAt(value, path, RUNNER_ROW).row;
}

/** The cycle status's `runner_commands`. */
export function parseRunnerCommands(
  value: unknown,
  path = 'runnerCommands',
): CommandRows<RunnerCommandRowKind> {
  return commandRowsAt(value, path, parseRunnerCommandRow);
}

function applyStepSourceAt(value: unknown, path: string, step: string | null): ApplyStepSource | null {
  if (value === null) return null;
  const record = objectAt(value, path);
  const status = stringAt(record.status, `${path}.status`);
  if (step === null || APPLY_STEP_SOURCE[step] !== status) {
    return fail(`${path}.status`, 'the status that carries this step');
  }
  if (status === 'runner_command') {
    exactKeysAt(record, ['status', 'kind', 'request_key'], path);
    if (record.kind !== 'reset') return fail(`${path}.kind`, 'reset');
    if (!isRequestKey(record.request_key)) return fail(`${path}.request_key`, '32 lowercase hex characters');
    return { status, kind: 'reset', requestKey: record.request_key };
  }
  if (status === 'activation_preparation') {
    exactKeysAt(record, ['status', 'captured_input_digest'], path);
    return {
      status,
      capturedInputDigest: digestAt(record.captured_input_digest, `${path}.captured_input_digest`),
    };
  }
  if (status === 'component_action') {
    exactKeysAt(record, ['status', 'component_id'], path);
    return { status, componentId: nonEmptyStringAt(record.component_id, `${path}.component_id`) };
  }
  exactKeysAt(record, ['status', 'apply_id', 'apply_generation'], path);
  return {
    status: status as 'manager_apply' | 'scene_clear',
    applyId: positiveIntegerAt(record.apply_id, `${path}.apply_id`),
    applyGeneration: positiveIntegerAt(record.apply_generation, `${path}.apply_generation`),
  };
}

const APPLY_ROW: RowSpec<ApplyCommandRowKind> = {
  kinds: APPLY_COMMAND_STEPS,
  outcomes: ['succeeded', 'refused', 'failed', 'unknown', 'interrupted'],
  extraKeys: ['step_source'],
  detailsWhenSucceeded: true,
};

export function parseApplyCommandRow(value: unknown, path = 'applyCommand'): ApplyCommandRow {
  const { row, record } = commandRowAt(value, path, APPLY_ROW);
  return { ...row, stepSource: applyStepSourceAt(record.step_source, `${path}.step_source`, row.step) };
}

/** An Apply's component rows, at most 256 (the History record's bound). */
const APPLY_COMPONENTS_MAXIMUM = 256;

/**
 * The ApplyResult an ended Apply-level row states: succeeded, or failed with
 * its typed code (any other end); its component rows are `details.components`.
 */
export function applyResultOfRow(row: ApplyCommandRow, path = 'applyCommand'): ApplyResult {
  const end = row.end;
  if (end === null) return fail(`${path}.end`, 'the end of an ended Apply');
  const components = end.details?.components;
  const parsed = components === undefined ? []
    : parseApplyResultComponents(
      boundedArrayAt(components, `${path}.end.details.components`, APPLY_COMPONENTS_MAXIMUM),
      `${path}.end.details.components`,
    );
  return end.code === null
    ? { outcome: 'succeeded', components: parsed }
    : { outcome: 'failed', components: parsed, error: { code: end.code, message: '' } };
}

/** `apply_commands` in Composer's shell and Deployment workspace. */
export function parseApplyCommands(value: unknown, path = 'applyCommands'): ApplyCommandRows {
  return commandRowsAt(value, path, parseApplyCommandRow);
}

const ACTIVATION_PREPARATION_ROW: RowSpec<'activation_preparation'> = {
  kinds: { activation_preparation: ACTIVATION_PREPARATION_STEPS },
  outcomes: ['succeeded', 'refused', 'failed'],
  extraKeys: ['captured_input_digest'],
};

export function parseActivationPreparationRow(
  value: unknown,
  path = 'activationPreparation',
): ActivationPreparationRow {
  const { row, record } = commandRowAt(value, path, ACTIVATION_PREPARATION_ROW);
  return {
    ...row,
    capturedInputDigest: digestAt(record.captured_input_digest, `${path}.captured_input_digest`),
  };
}

/** Headless's `activation_preparation` beside `scene_clear` in its cycle-status route. */
export function parseActivationPreparationStatus(
  value: unknown,
  path = 'activationPreparation',
): CommandRows<'activation_preparation', ActivationPreparationRow> {
  return commandRowsAt(value, path, parseActivationPreparationRow);
}

/**
 * A reply at acceptance: the command's row as it stands (a replayed key may
 * return a row that already ended, accepted or not).
 */
function commandReplyAt<Row extends CommandRow>(
  value: unknown,
  path: string,
  schemaVersion: string,
  rowAt: (value: unknown, path: string) => Row,
  optional: readonly string[] = [],
): Row {
  const record = objectAt(value, path);
  allowedKeysAt(record, ['schema_version', 'command'], optional, path);
  if (record.schema_version !== schemaVersion) return fail(`${path}.schema_version`, schemaVersion);
  return rowAt(record.command, `${path}.command`);
}

/**
 * A runner command's reply at acceptance: its row as it stands. Headless
 * adds the start `A` and the key's display form (contract 1.5).
 */
export function parseProgramCommandReply(
  value: unknown,
  path = 'programCommandReply',
): CommandRow<RunnerCommandRowKind> {
  const record = objectAt(value, path);
  if (record.application_start_id !== undefined && !isApplicationStartId(record.application_start_id)) {
    return fail(`${path}.application_start_id`, '16 lowercase hex characters');
  }
  if (record.display !== undefined) nonEmptyStringAt(record.display, `${path}.display`);
  return commandReplyAt(
    value, path, 'manyforge.program_command_reply.v1', parseRunnerCommandRow,
    ['application_start_id', 'display'],
  );
}

/** An Apply, Revert, Load or New reply at acceptance: its row as it stands. */
export function parseApplyCommandReply(value: unknown, path = 'applyCommandReply'): ApplyCommandRow {
  return commandReplyAt(value, path, 'manyforge.apply_command_reply.v1', parseApplyCommandRow);
}

/** The row a client's own request key names, in progress or ended; null when not kept. */
export function commandRowWithKey<Row extends CommandRow>(
  rows: CommandRows<string, Row> | null | undefined,
  requestKey: string,
): Row | null {
  if (rows === null || rows === undefined) return null;
  if (rows.inProgress?.requestKey === requestKey) return rows.inProgress;
  return rows.ended.find((row) => row.requestKey === requestKey) ?? null;
}
