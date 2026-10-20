import type {
  JsonValue,
  NodeEnvironmentResponse,
  NodeImplementationAvailability,
  PersistentKeyValue,
  PersistentKeyValueEdit,
  PersistentKeyValues,
  PersistentKeyValuesCleared,
  ExpressionCheckResult,
  ExpressionDiagnostic,
  ExpressionValueType,
  ProgramCycleStatus,
  ProgramRunStateName,
  ProgramRunStatus,
  FailureDetails,
  FailureOrigin,
  StructuredError,
  ProgramExecutionSelectionControls,
  CycleRecoveryAction,
  CycleRecoveryActionId,
  CycleRecoveryOffer,
  ProgramRecoveryHold,
  ProgramRuntimeControlProjection,
  ProgramRuntimeControlsSnapshot,
  RuntimeControlAuditEvent,
  AutomaticRetryAuditEvent,
  AutomaticRetryAuditFacts,
  TakeoverFallbackAuditEvent,
  TakeoverFallbackAuditFacts,
} from '../../types/api';
import type { RuntimeScaleControlAccepted, RuntimeScaleControlStatus, ProgramRunAdmittedSpeeds } from '../../types/robotControl';
import type { JsonObject } from './shared';
import {
  allowedKeysAt,
  arrayAt,
  booleanAt,
  boundedArrayAt,
  digestAt,
  exactKeysAt,
  fail,
  integerAt,
  jsonRecordAt,
  jsonValueAt,
  monotonicNanosecondsAt,
  nonEmptyStringAt,
  nullableMonotonicNanosecondsAt,
  nullableNumberAt,
  nullablePositiveUint64TextAt,
  nullableStringAt,
  numberAt,
  objectAt,
  positiveIntegerAt,
  stringArrayAt,
  stringAt,
} from './shared';

export function parseProgramCycleStatus(value: unknown): ProgramCycleStatus {
  const record = objectAt(value, 'programCycleStatus');
  exactKeysAt(record, [
    'program_runner_owner_epoch', 'cycle_generation', 'state', 'program_run_id',
    'cycle_claim_generation', 'terminal_or_settlement_reference',
    'run_revision', 'node_status_revision',
    'runtime_control_revision', 'recovery_offer', 'run', 'runtime_controls',
    'execution_control_revision', 'causal_correlation_id',
    'causal_session_id', 'causal_client_sequence', 'current_command_kind',
    'failure', 'reset_phase', 'reset_options', 'reset_robot_position', 'allowed_commands',
  ], 'programCycleStatus');
  const state = stringAt(record.state, 'programCycleStatus.state');
  if (!['idle_after_reset', 'starting', 'running', 'stopping',
    'resetting', 'terminal_awaiting_reset', 'recovery_required'].includes(state)) {
    return fail('programCycleStatus.state', 'a supported Program cycle state');
  }
  const parsed: ProgramCycleStatus = {
    programRunnerOwnerEpoch: stringAt(
      record.program_runner_owner_epoch,
      'programCycleStatus.program_runner_owner_epoch',
    ),
    cycleGeneration: positiveIntegerAt(
      record.cycle_generation,
      'programCycleStatus.cycle_generation',
    ),
    state: state as ProgramCycleStatus['state'],
    programRunId: nullableStringAt(
      record.program_run_id,
      'programCycleStatus.program_run_id',
    ),
    cycleClaimGeneration: record.cycle_claim_generation === null
      ? null
      : positiveIntegerAt(
        record.cycle_claim_generation,
        'programCycleStatus.cycle_claim_generation',
      ),
    terminalOrSettlementReference:
      record.terminal_or_settlement_reference === null
        ? null
        : digestAt(
          record.terminal_or_settlement_reference,
          'programCycleStatus.terminal_or_settlement_reference',
        ),
    runRevision: record.run_revision === null
      ? null
      : positiveIntegerAt(record.run_revision, 'programCycleStatus.run_revision'),
    nodeStatusRevision: record.node_status_revision === null
      ? null
      : positiveIntegerAt(
        record.node_status_revision,
        'programCycleStatus.node_status_revision',
      ),
    runtimeControlRevision: record.runtime_control_revision === null
      ? null
      : positiveIntegerAt(
        record.runtime_control_revision,
        'programCycleStatus.runtime_control_revision',
      ),
    executionControlRevision: positiveIntegerAt(
      record.execution_control_revision,
      'programCycleStatus.execution_control_revision',
    ),
    causalCorrelationId: nullableStringAt(
      record.causal_correlation_id,
      'programCycleStatus.causal_correlation_id',
    ),
    causalSessionId: nullableStringAt(
      record.causal_session_id,
      'programCycleStatus.causal_session_id',
    ),
    causalClientSequence: nullablePositiveUint64TextAt(
      record.causal_client_sequence,
      'programCycleStatus.causal_client_sequence',
    ),
    currentCommandKind: nullableStringAt(
      record.current_command_kind,
      'programCycleStatus.current_command_kind',
    ),
    failure: record.failure === null
      ? null
      : jsonRecordAt(record.failure, 'programCycleStatus.failure'),
    resetPhase: nullableStringAt(
      record.reset_phase,
      'programCycleStatus.reset_phase',
    ),
    resetOptions: record.reset_options === null
      ? null
      : (() => {
        const path = 'programCycleStatus.reset_options';
        const options = objectAt(record.reset_options, path);
        exactKeysAt(options, ['reload_initial_scene', 'reset_robot_position'], path);
        return {
          reloadInitialScene: booleanAt(options.reload_initial_scene, `${path}.reload_initial_scene`),
          resetRobotPosition: booleanAt(options.reset_robot_position, `${path}.reset_robot_position`),
        };
      })(),
    resetRobotPosition: (() => {
      const path = 'programCycleStatus.reset_robot_position';
      const capability = objectAt(record.reset_robot_position, path);
      exactKeysAt(capability, ['available', 'reason'], path);
      const available = booleanAt(capability.available, `${path}.available`);
      const reason = capability.reason === null
        ? null : nonEmptyStringAt(capability.reason, `${path}.reason`);
      if (available !== (reason === null)) {
        return fail(path, 'available with no reason, or unavailable with a reason');
      }
      return { available, reason };
    })(),
    allowedCommands: arrayAt(
      record.allowed_commands,
      'programCycleStatus.allowed_commands',
    ).map((value, index) => {
      const path = `programCycleStatus.allowed_commands[${index}]`;
      const command = objectAt(value, path);
      exactKeysAt(command, [
        'command_kind', 'semantic_target', 'allowed', 'disabled_reason',
      ], path);
      return {
        commandKind: stringAt(command.command_kind, `${path}.command_kind`),
        semanticTarget: jsonRecordAt(command.semantic_target, `${path}.semantic_target`),
        allowed: booleanAt(command.allowed, `${path}.allowed`),
        disabledReason: nullableStringAt(
          command.disabled_reason,
          `${path}.disabled_reason`,
        ),
      };
    }),
    run: record.run === null ? null : parseProgramRunStatus(record.run),
    runtimeControls: record.runtime_controls === null
      ? null
      : parseProgramRuntimeControlProjection(record.runtime_controls),
  };
  if (parsed.resetOptions !== null && (
    !['resetting', 'recovery_required'].includes(parsed.state)
    || parsed.currentCommandKind !== 'program_reset'
  )) {
    return fail('programCycleStatus.reset_options', 'an accepted Reset intent');
  }
  if (parsed.state === 'resetting' && parsed.resetOptions === null) {
    return fail('programCycleStatus.reset_options', 'the accepted Reset selectors');
  }
  if (parsed.resetOptions !== null) {
    if (
      (parsed.resetPhase === 'restoring_scene' && !parsed.resetOptions.reloadInitialScene)
      || (parsed.resetPhase === 'restoring_position' && !parsed.resetOptions.resetRobotPosition)
    ) {
      return fail('programCycleStatus.reset_phase', 'a phase selected by the accepted Reset');
    }
    if (parsed.programRunId === null && (
      parsed.cycleClaimGeneration !== null
      || parsed.terminalOrSettlementReference !== null
      || !['restoring_scene', 'restoring_position', 'releasing'].includes(parsed.resetPhase ?? '')
      || !(parsed.resetOptions.reloadInitialScene || parsed.resetOptions.resetRobotPosition)
    )) {
      return fail('programCycleStatus.reset_options', 'a selected no-run Reset with no claim or terminal reference');
    }
    if (parsed.programRunId === null && parsed.state === 'recovery_required'
      && parsed.failure?.failed_phase !== parsed.resetPhase) {
      return fail('programCycleStatus.failure', 'the exact failed phase of the selected no-run Reset');
    }
  }
  if (parsed.run === null) {
    if (parsed.runRevision !== null || parsed.nodeStatusRevision !== null) {
      return fail('programCycleStatus.run', 'the advertised run revisions');
    }
  } else if (
    parsed.run.capture.programRunId !== parsed.programRunId
    || (
      !['terminal_awaiting_reset', 'resetting'].includes(parsed.state)
      && parsed.run.ownerEpoch !== parsed.programRunnerOwnerEpoch
    )
    || parsed.run.runRevision !== parsed.runRevision
    || parsed.run.nodeStatusRevision !== parsed.nodeStatusRevision
  ) {
    return fail('programCycleStatus.run', 'the exact advertised run projection');
  }
  if (parsed.runtimeControls === null) {
    if (parsed.runtimeControlRevision !== null) {
      return fail(
        'programCycleStatus.runtimeControls',
        'the advertised runtime-control revision',
      );
    }
  } else if (
    parsed.run === null
    || parsed.runtimeControls.programRunId !== parsed.programRunId
    || parsed.runtimeControls.ownerEpoch !== parsed.programRunnerOwnerEpoch
    || parsed.runtimeControls.runRevision !== parsed.runRevision
    || parsed.runtimeControls.runtimeControls.runtimeControlRevision
      !== parsed.runtimeControlRevision
  ) {
    return fail(
      'programCycleStatus.runtimeControls',
      'the exact advertised runtime-control projection',
    );
  }
  return parsed;
}

function runtimeScalePairIdentity(componentRoleId: string, targetRoleId: string): string {
  return JSON.stringify([componentRoleId, targetRoleId]);
}

const PROGRAM_RUN_STATES = new Set<ProgramRunStateName>([
  'running',
  'intervention_required',
  'full_stopping',
  'full_stopped',
  'stopping',
  'finalizing',
  'stopped',
  'succeeded',
  'failed',
]);

/**
 * Registered failure-detail keys (spec error contract §1.1). A run record
 * carries no other key; the message already names any other cause.
 */
const FAILURE_DETAIL_KEYS: ReadonlySet<string> = new Set([
  // Owner refusals.
  'reason', 'reference_type', 'reference_id', 'target_role_id',
  // Node input failures.
  'input_problem', 'path', 'source_kind', 'source_reference',
  // Required ErrorDTO details a run may surface.
  'latch_id', 'retryable', 'required_action',
]);

const FAILURE_DETAIL_MAX_ENTRIES = 16;
const FAILURE_DETAIL_MAX_CANONICAL_BYTES = 2048;
const FAILURE_TEXT_MAX_BYTES = 256;

function utf8Length(text: string): number {
  return new TextEncoder().encode(text).length;
}

function failureDetailsAt(value: unknown, path: string): FailureDetails | null {
  if (value === null) return null;
  const record = objectAt(value, path);
  const keys = Object.keys(record).sort();
  if (keys.length > FAILURE_DETAIL_MAX_ENTRIES) {
    return fail(path, `at most ${FAILURE_DETAIL_MAX_ENTRIES} failure details`);
  }
  const details: FailureDetails = {};
  for (const key of keys) {
    const item = record[key];
    if (!FAILURE_DETAIL_KEYS.has(key)) {
      return fail(`${path}.${key}`, 'a registered failure-detail key');
    }
    if (
      !(item === null || typeof item === 'boolean'
        || (typeof item === 'number' && Number.isFinite(item)
          && (!Number.isInteger(item) || Number.isSafeInteger(item)))
        || (typeof item === 'string' && utf8Length(item) <= FAILURE_TEXT_MAX_BYTES))
    ) {
      return fail(
        `${path}.${key}`,
        `a scalar failure detail (text of at most ${FAILURE_TEXT_MAX_BYTES} bytes, safe integers)`,
      );
    }
    details[key] = item;
  }
  // Sorted keys and scalar values: JSON.stringify is the RFC 8785 canonical form.
  if (utf8Length(JSON.stringify(details)) > FAILURE_DETAIL_MAX_CANONICAL_BYTES) {
    return fail(path, `failure details of at most ${FAILURE_DETAIL_MAX_CANONICAL_BYTES} bytes`);
  }
  return details;
}

const FAILURE_MESSAGE_MAX_BYTES = 4096;

function failureMessageAt(value: unknown, path: string): string {
  const message = stringAt(value, path);
  if (utf8Length(message) > FAILURE_MESSAGE_MAX_BYTES) {
    return fail(path, `a failure message of at most ${FAILURE_MESSAGE_MAX_BYTES} bytes`);
  }
  return message;
}

function failureIdentityAt(value: unknown, path: string): string | null {
  if (value === null) return null;
  const text = stringAt(value, path);
  if (text.length === 0 || utf8Length(text) > FAILURE_TEXT_MAX_BYTES) {
    return fail(path, `a nonempty identity of at most ${FAILURE_TEXT_MAX_BYTES} bytes`);
  }
  return text;
}

function failureOriginAt(value: unknown, path: string): FailureOrigin | null {
  if (value === null) return null;
  const record = objectAt(value, path);
  exactKeysAt(record, ['kind', 'node_instance_id', 'owner_api'], path);
  const kind = stringAt(record.kind, `${path}.kind`);
  const nodeInstanceId = failureIdentityAt(
    record.node_instance_id,
    `${path}.node_instance_id`,
  );
  const ownerApi = failureIdentityAt(record.owner_api, `${path}.owner_api`);
  if (
    !(kind === 'program_node' && nodeInstanceId !== null && ownerApi === null)
    && !(kind === 'owner' && nodeInstanceId !== null)
    && !(kind === 'program_runner' && ownerApi === null)
  ) {
    return fail(
      path,
      'a program_node (node, no owner API), owner (node) or program_runner (no owner API) origin',
    );
  }
  return { kind: kind as FailureOrigin['kind'], nodeInstanceId, ownerApi };
}

/** The bounded `details` and `origin` of one run record (details need an origin). */
function failureContextAt(
  record: JsonObject,
  path: string,
): Pick<StructuredError, 'details' | 'origin'> {
  const details = failureDetailsAt(record.details, `${path}.details`);
  const origin = failureOriginAt(record.origin, `${path}.origin`);
  if (details !== null && origin === null) {
    return fail(`${path}.details`, 'null unless the record names a failure origin');
  }
  return { details, origin };
}

/**
 * The ErrorDTO (`{code, message, details}`) a refused Composer API request
 * returned in its `detail`, or null when the body carries none. The request
 * itself is the origin. Details are the code's own key/value payload; only
 * their scalar values are carried, nested values stay in the raw payload.
 */
export function parseApiErrorDto(payload: unknown): StructuredError | null {
  if (payload === null || typeof payload !== 'object' || Array.isArray(payload)) {
    return null;
  }
  const candidate = (payload as JsonObject).detail;
  if (
    candidate === null
    || typeof candidate !== 'object'
    || Array.isArray(candidate)
    || typeof (candidate as JsonObject).code !== 'string'
    || (candidate as JsonObject).code === ''
  ) {
    return null;
  }
  const dto = candidate as JsonObject & { code: string };
  const rawDetails = dto.details;
  let details: FailureDetails | null = null;
  if (rawDetails !== null && typeof rawDetails === 'object' && !Array.isArray(rawDetails)) {
    details = Object.fromEntries(Object.entries(rawDetails as JsonObject)
      .filter((entry): entry is [string, string | number | boolean | null] => {
        const item = entry[1];
        return item === null || typeof item === 'string' || typeof item === 'boolean'
          || (typeof item === 'number' && Number.isFinite(item));
      }));
  }
  return {
    code: dto.code,
    message: typeof dto.message === 'string' ? dto.message : '',
    details,
    origin: null,
  };
}

/** Known Run outcomes retain the closed ErrorDTO carrier and native flat facts. */
export function parseKnownProgramRunOutcome(payload: unknown, status: number): StructuredError {
  const path = 'programRunOutcome';
  const envelope = objectAt(payload, path);
  exactKeysAt(envelope, ['detail'], path);
  const dto = objectAt(envelope.detail, `${path}.detail`);
  allowedKeysAt(dto, ['code', 'message', 'details'], ['request_id'], `${path}.detail`);
  const code = stringAt(dto.code, `${path}.code`);
  const beforeStart = code === 'program_run_stopped_before_start';
  const stopped = beforeStart || code === 'program_run_superseded_by_stop';
  if ((!stopped && code !== 'program_run_started_then_failed')
    || status !== (beforeStart ? 409 : 422)) {
    return fail(path, 'a known Run outcome and its matching HTTP status');
  }
  if (dto.request_id !== undefined) failureIdentityAt(dto.request_id, `${path}.request_id`);
  const details = objectAt(dto.details, `${path}.details`);
  const keys = Object.keys(details);
  if (keys.length > FAILURE_DETAIL_MAX_ENTRIES) {
    return fail(path, `at most ${FAILURE_DETAIL_MAX_ENTRIES} outcome details`);
  }
  const parsed: FailureDetails = {};
  for (const key of keys) {
    const value = details[key];
    if (utf8Length(key) > 64 || key.length === 0
      || !(value === null || typeof value === 'boolean'
        || (typeof value === 'number' && Number.isFinite(value)
          && (!Number.isInteger(value) || Number.isSafeInteger(value)))
        || (typeof value === 'string'
          && utf8Length(value) <= (key === 'cause_message' ? FAILURE_MESSAGE_MAX_BYTES : FAILURE_TEXT_MAX_BYTES)))) {
      return fail(`${path}.details.${key}`, 'a bounded flat ErrorDTO detail');
    }
    parsed[key] = value as string | number | boolean | null;
  }
  const runId = failureIdentityAt(details.program_run_id, `${path}.program_run_id`);
  if (runId === null || details.needs_reset !== true) {
    return fail(path, 'the actual run identity and needs_reset:true');
  }
  if (beforeStart) {
    if (details.claim_generation !== null || details.no_effect !== true) {
      return fail(path, 'no claim and proven no effect before start');
    }
  } else {
    positiveIntegerAt(details.claim_generation, `${path}.claim_generation`);
    if (details.no_effect !== undefined) return fail(path, 'no preclaim effect assertion');
  }
  if (stopped ? details.stopped_by_operator !== true : details.stopped_by_operator !== undefined) {
    return fail(path, 'the Stop discriminant only on a stopped outcome');
  }
  const cause = failureIdentityAt(details.cause_code, `${path}.cause_code`);
  if (cause === null) return fail(path, 'the named owner cause');
  if (stopped && cause !== 'program_run_superseded_by_stop') {
    return fail(path, 'the actual operator Stop cause');
  }
  const causeMessage = failureMessageAt(details.cause_message, `${path}.cause_message`);
  if (causeMessage.length === 0) return fail(path, 'the owner cause message');
  const message = failureMessageAt(dto.message, `${path}.message`);
  if (message.length === 0) return fail(path, 'the owner outcome message');
  return { code, message, details: parsed, origin: null };
}

function programOperationFailuresAt(value: unknown, path: string): ProgramRunStatus['operationFailures'] {
  // Match the owner/Headless provenance contract, including UTF-8 byte bounds.
  const textAt = (value: unknown, path: string, maximumBytes: number, empty = false): string => {
    const text = stringAt(value, path);
    if ((!empty && text.length === 0) || utf8Length(text) > maximumBytes
      || text.normalize('NFC') !== text) {
      return fail(path, `NFC text within ${maximumBytes} UTF-8 bytes${empty ? '' : ', nonempty'}`);
    }
    return text;
  };
  const before = (left: string, right: string): boolean => {
    // The producer sorts Unicode code points (Python), not UTF-16 code units.
    const a = Array.from(left); const b = Array.from(right);
    for (let index = 0; index < Math.min(a.length, b.length); index += 1) {
      const difference = a[index].codePointAt(0)! - b[index].codePointAt(0)!;
      if (difference !== 0) return difference < 0;
    }
    return a.length < b.length;
  };
  return boundedArrayAt(value, path, 4096).map((value, index) => {
    const key = `${path}[${index}]`;
    const row = objectAt(value, key);
    exactKeysAt(row, ['node_instance_id', 'node_attempt_id', 'operator_facing_run_revision',
      'operation', 'resource_ids', 'failure', 'recovery_class', 'terminal_facts'], key);
    const operation = objectAt(row.operation, `${key}.operation`);
    exactKeysAt(operation, ['source_owner_epoch', 'operation_id', 'operation_revision'], `${key}.operation`);
    const failure = objectAt(row.failure, `${key}.failure`);
    exactKeysAt(failure, ['code', 'message'], `${key}.failure`);
    const resourceIds = boundedArrayAt(row.resource_ids, `${key}.resource_ids`, 4096)
      .map((value, index) => textAt(value, `${key}.resource_ids[${index}]`, 1024));
    if (resourceIds.length === 0 || resourceIds.some((value, index) => index > 0 && !before(resourceIds[index - 1], value))) {
      return fail(`${key}.resource_ids`, 'nonempty sorted unique resource IDs');
    }
    const terminalFacts = boundedArrayAt(row.terminal_facts, `${key}.terminal_facts`, 4096)
      .map((value, index) => {
        const factPath = `${key}.terminal_facts[${index}]`;
        const fact = objectAt(value, factPath);
        exactKeysAt(fact, ['kind', 'effect'], factPath);
        return { kind: textAt(fact.kind, `${factPath}.kind`, 128),
          effect: textAt(fact.effect, `${factPath}.effect`, 128) };
      });
    return {
      nodeInstanceId: textAt(row.node_instance_id, `${key}.node_instance_id`, 128),
      nodeAttemptId: textAt(row.node_attempt_id, `${key}.node_attempt_id`, 512),
      operatorFacingRunRevision: positiveIntegerAt(row.operator_facing_run_revision, `${key}.operator_facing_run_revision`),
      operation: { sourceOwnerEpoch: textAt(operation.source_owner_epoch, `${key}.operation.source_owner_epoch`, 2048),
        operationId: textAt(operation.operation_id, `${key}.operation.operation_id`, 2048),
        operationRevision: positiveIntegerAt(operation.operation_revision, `${key}.operation.operation_revision`) },
      resourceIds,
      failure: { code: textAt(failure.code, `${key}.failure.code`, 256),
        message: textAt(failure.message, `${key}.failure.message`, 4096, true) },
      recoveryClass: textAt(row.recovery_class, `${key}.recovery_class`, 128),
      terminalFacts,
    };
  });
}

export function parseProgramRunStatus(value: unknown): ProgramRunStatus {
  const path = 'programRun';
  const record = objectAt(value, path);
  exactKeysAt(
    record,
    [
      'active_operation_count',
      'operation_failures',
      'capture',
      'details',
      'message',
      'node_status_revision',
      'node_statuses',
      'origin',
      'owner_epoch',
      'owner_id',
      'reason_code',
      'run_revision',
      'schema_version',
      'state',
      'tree_status',
    ],
    path,
  );
  const schemaVersion = stringAt(record.schema_version, `${path}.schema_version`);
  if (schemaVersion !== 'manyforge.program_run_status.v3') {
    return fail(`${path}.schema_version`, 'manyforge.program_run_status.v3');
  }
  const state = stringAt(record.state, `${path}.state`) as ProgramRunStateName;
  if (!PROGRAM_RUN_STATES.has(state)) {
    return fail(`${path}.state`, 'a supported Program-run state');
  }
  const capture = objectAt(record.capture, `${path}.capture`);
  exactKeysAt(
    capture,
    [
      'applied_record_digest',
      'graph_revision',
      'node_environment_digest',
      'node_policies',
      'node_policy_digest',
      'owner_identities',
      'program_binding_id',
      'program_id',
      'program_revision',
      'program_run_id',
      'retained_bundle_id',
      'source_document_digest',
      'scenario_id',
      'schema_version',
    ],
    `${path}.capture`,
  );
  if (capture.schema_version !== 'manyforge.program_run_capture.v2') {
    return fail(`${path}.capture.schema_version`, 'manyforge.program_run_capture.v2');
  }
  const ownerIdentities = boundedArrayAt(
    capture.owner_identities,
    `${path}.capture.owner_identities`,
    4096,
  ).map((value, index) => {
    const ownerPath = `${path}.capture.owner_identities[${index}]`;
    const owner = objectAt(value, ownerPath);
    exactKeysAt(owner, ['owner_epoch', 'owner_id', 'owner_role'], ownerPath);
    return {
      ownerRole: stringAt(owner.owner_role, `${ownerPath}.owner_role`),
      ownerId: stringAt(owner.owner_id, `${ownerPath}.owner_id`),
      ownerEpoch: stringAt(owner.owner_epoch, `${ownerPath}.owner_epoch`),
    };
  });
  const nodePolicies = boundedArrayAt(
    capture.node_policies,
    `${path}.capture.node_policies`,
    100_000,
  ).map((value, index) => jsonValueAt(
    value,
    `${path}.capture.node_policies[${index}]`,
  ));
  const ownerRoles = ownerIdentities.map((owner) => owner.ownerRole);
  if (
    !ownerRoles.includes('program_runner')
    || ownerRoles.some((role, index) => index > 0 && role <= ownerRoles[index - 1])
  ) {
    return fail(
      `${path}.capture.owner_identities`,
      'strictly role-sorted unique owner identities including program_runner',
    );
  }
  const treeStatus = stringAt(record.tree_status, `${path}.tree_status`);
  if (!['invalid', 'running', 'success', 'failure'].includes(treeStatus)) {
    return fail(`${path}.tree_status`, 'a supported tree status');
  }
  const rawNodeStatuses = objectAt(record.node_statuses, `${path}.node_statuses`);
  const nodeStatusEntries = Object.entries(rawNodeStatuses);
  if (nodeStatusEntries.length > 100_000) {
    return fail(`${path}.node_statuses`, 'at most 100000 authored-node statuses');
  }
  const nodeStatuses: ProgramRunStatus['nodeStatuses'] = {};
  for (const [instanceId, rawStatus] of nodeStatusEntries) {
    const status = stringAt(rawStatus, `${path}.node_statuses.${instanceId}`);
    if (
      instanceId.length === 0
      || instanceId.length > 512
      || !['idle', 'running', 'held', 'success', 'failure', 'interrupted'].includes(status)
    ) {
      return fail(
        `${path}.node_statuses.${instanceId}`,
        'one bounded authored-node ID and supported presentation status',
      );
    }
    nodeStatuses[instanceId] = status as ProgramRunStatus['nodeStatuses'][string];
  }
  const ownerId = stringAt(record.owner_id, `${path}.owner_id`);
  const ownerEpoch = stringAt(record.owner_epoch, `${path}.owner_epoch`);
  const runnerIdentity = ownerIdentities.find(
    (owner) => owner.ownerRole === 'program_runner',
  );
  if (
    runnerIdentity?.ownerId !== ownerId
    || runnerIdentity.ownerEpoch !== ownerEpoch
  ) {
    return fail(
      `${path}.capture.owner_identities`,
      'a program_runner identity matching the status owner',
    );
  }
  const activeOperationCount = record.active_operation_count === null
    ? null
    : integerAt(record.active_operation_count, `${path}.active_operation_count`);
  if (
    activeOperationCount !== null
    && (activeOperationCount < 0 || activeOperationCount > 4096)
  ) {
    return fail(`${path}.active_operation_count`, 'an integer from 0 through 4096');
  }
  const reasonCode = nullableStringAt(record.reason_code, `${path}.reason_code`);
  const message = stringAt(record.message, `${path}.message`);
  const failureContext = failureContextAt(record, path);
  if (reasonCode === null && failureContext.origin !== null) {
    return fail(`${path}.origin`, 'null when the run names no reason');
  }
  return {
    schemaVersion,
    ownerId,
    ownerEpoch,
    runRevision: positiveIntegerAt(record.run_revision, `${path}.run_revision`),
    state,
    capture: {
      programRunId: stringAt(capture.program_run_id, `${path}.capture.program_run_id`),
      appliedRecordDigest: digestAt(
        capture.applied_record_digest,
        `${path}.capture.applied_record_digest`,
      ),
      scenarioId: stringAt(capture.scenario_id, `${path}.capture.scenario_id`),
      sourceDocumentDigest: digestAt(
        capture.source_document_digest,
        `${path}.capture.source_document_digest`,
      ),
      programId: stringAt(capture.program_id, `${path}.capture.program_id`),
      programRevision: digestAt(
        capture.program_revision,
        `${path}.capture.program_revision`,
      ),
      programBindingId: digestAt(
        capture.program_binding_id,
        `${path}.capture.program_binding_id`,
      ),
      retainedBundleId: digestAt(
        capture.retained_bundle_id,
        `${path}.capture.retained_bundle_id`,
      ),
      graphRevision: digestAt(
        capture.graph_revision,
        `${path}.capture.graph_revision`,
      ),
      nodeEnvironmentDigest: digestAt(
        capture.node_environment_digest,
        `${path}.capture.node_environment_digest`,
      ),
      nodePolicies,
      nodePolicyDigest: digestAt(
        capture.node_policy_digest,
        `${path}.capture.node_policy_digest`,
      ),
      ownerIdentities,
    },
    treeStatus: treeStatus as ProgramRunStatus['treeStatus'],
    nodeStatusRevision: positiveIntegerAt(
      record.node_status_revision,
      `${path}.node_status_revision`,
    ),
    nodeStatuses,
    activeOperationCount,
    operationFailures: programOperationFailuresAt(record.operation_failures, `${path}.operation_failures`),
    reason: reasonCode === null
      ? null
      : { code: reasonCode, message, ...failureContext },
    message,
  };
}

const RECOVERY_ACTION_IDS = new Set<CycleRecoveryActionId>([
  'retry_motion_from_current_state',
  'acknowledge_and_continue',
  'retry_after_scene_repair',
  'replan_from_stopped_state',
  'retry_runtime_step',
  'continue_with_failure',
  'reset_cycle',
]);

function nullablePositiveIntegerAt(value: unknown, path: string): number | null {
  return value === null ? null : positiveIntegerAt(value, path);
}

/** The one event kind whose facts are parsed into typed fields. */
export function isAutomaticRetryAuditEvent(
  event: RuntimeControlAuditEvent,
): event is AutomaticRetryAuditEvent {
  return event.kind === 'program_node_automatic_retry';
}

function parseAutomaticRetryAuditFacts(
  record: JsonObject,
  path: string,
): AutomaticRetryAuditFacts {
  const attempt = positiveIntegerAt(record.attempt, `${path}.attempt`);
  const maxAttempts = positiveIntegerAt(record.max_attempts, `${path}.max_attempts`);
  if (attempt < 2 || attempt > maxAttempts) {
    return fail(path, 'attempt from 2 through max_attempts');
  }
  const delayS = numberAt(record.delay_s, `${path}.delay_s`);
  if (delayS < 0) return fail(`${path}.delay_s`, 'a non-negative number');
  const code = record.code === undefined || record.code === null
    ? null
    : nonEmptyStringAt(record.code, `${path}.code`);
  const message = record.message === undefined || record.message === null
    ? null
    : nonEmptyStringAt(record.message, `${path}.message`);
  if ((code === null) !== (message === null)) {
    return fail(path, 'a Kernel failure with both code and message, or neither');
  }
  return {
    nodeInstanceId: nonEmptyStringAt(record.node_instance_id, `${path}.node_instance_id`),
    attempt,
    maxAttempts,
    cause: nonEmptyStringAt(record.cause, `${path}.cause`),
    delayS,
    failure: code === null || message === null
      ? null
      : { code, message, details: null, origin: null },
  };
}

export function isTakeoverFallbackAuditEvent(
  event: RuntimeControlAuditEvent,
): event is TakeoverFallbackAuditEvent {
  return event.kind === 'program_node_takeover_fallback';
}

function parseTakeoverFallbackAuditFacts(
  record: JsonObject,
  path: string,
): TakeoverFallbackAuditFacts {
  const takeoverPath = stringAt(record.path, `${path}.path`);
  if (takeoverPath !== 'spring_back_hold' && takeoverPath !== 'ordinary_containment') {
    return fail(`${path}.path`, 'spring_back_hold or ordinary_containment');
  }
  const code = record.code === null
    ? null
    : nonEmptyStringAt(record.code, `${path}.code`);
  const message = record.message === null
    ? null
    : stringAt(record.message, `${path}.message`);
  if ((code === null) !== (message === null)) {
    return fail(path, 'a Kernel cause with both code and message, or neither');
  }
  const failureStage = record.failure_stage === undefined || record.failure_stage === null
    ? null
    : nonEmptyStringAt(record.failure_stage, `${path}.failure_stage`);
  const preparationMs = numberAt(record.preparation_ms, `${path}.preparation_ms`);
  if (preparationMs < 0) return fail(`${path}.preparation_ms`, 'a non-negative number');
  const budgetMs = numberAt(record.budget_ms, `${path}.budget_ms`);
  if (budgetMs <= 0) return fail(`${path}.budget_ms`, 'a positive number');
  return {
    nodeInstanceId: nonEmptyStringAt(record.node_instance_id, `${path}.node_instance_id`),
    incumbentOperationId: nonEmptyStringAt(
      record.incumbent_operation_id,
      `${path}.incumbent_operation_id`,
    ),
    path: takeoverPath,
    code,
    message,
    failureStage,
    preparationMs,
    budgetMs,
  };
}

function parseRuntimeControlAuditEvent(
  value: unknown,
  path: string,
): RuntimeControlAuditEvent {
  const record = jsonRecordAt(value, path);
  const eventRevision = positiveIntegerAt(
    record.event_revision,
    `${path}.event_revision`,
  );
  const kind = stringAt(record.kind, `${path}.kind`);
  if (kind === 'program_node_automatic_retry') {
    return {
      eventRevision,
      kind,
      facts: parseAutomaticRetryAuditFacts(record, path),
    };
  }
  if (kind === 'program_node_takeover_fallback') {
    return {
      eventRevision,
      kind,
      facts: parseTakeoverFallbackAuditFacts(record, path),
    };
  }
  return {
    eventRevision,
    kind,
    facts: Object.fromEntries(
      Object.entries(record).filter(([key]) => (
        key !== 'event_revision' && key !== 'kind'
      )),
    ),
  };
}

export function parseProgramRuntimeControlsSnapshot(
  value: unknown,
  path = 'programRuntimeControls',
): ProgramRuntimeControlsSnapshot {
  const record = objectAt(value, path);
  exactKeysAt(record, [
    'schema_version', 'program_run_id', 'runtime_control_revision',
    'recovery_fences', 'retry_counts', 'audit',
  ], path);
  const schemaVersion = stringAt(record.schema_version, `${path}.schema_version`);
  if (schemaVersion !== 'manyforge.program_runtime_controls.v3') {
    return fail(`${path}.schema_version`, 'manyforge.program_runtime_controls.v3');
  }
  const programRunId = stringAt(record.program_run_id, `${path}.program_run_id`);
  const fences = objectAt(record.recovery_fences, `${path}.recovery_fences`);
  exactKeysAt(fences, [
    'program_generation', 'deployment_generation', 'scene_admission_epoch',
    'pose_source_apply_generation',
  ], `${path}.recovery_fences`);
  // retry_counts stays on the wire (exact key) but has no Composer reader.
  objectAt(record.retry_counts, `${path}.retry_counts`);
  const revision = positiveIntegerAt(
    record.runtime_control_revision,
    `${path}.runtime_control_revision`,
  );
  const audit = boundedArrayAt(record.audit, `${path}.audit`, 256)
    .map((item, index) => parseRuntimeControlAuditEvent(
      item,
      `${path}.audit[${index}]`,
    ));
  if (
    audit.length !== Math.min(revision - 1, 256)
    || audit.some((event, index) => event.eventRevision !== revision - audit.length + index)
  ) {
    return fail(path, 'internally consistent run-scoped controls');
  }
  return {
    schemaVersion,
    programRunId,
    runtimeControlRevision: revision,
    recoveryFences: {
      programGeneration: integerAt(
        fences.program_generation,
        `${path}.recovery_fences.program_generation`,
      ),
      deploymentGeneration: integerAt(
        fences.deployment_generation,
        `${path}.recovery_fences.deployment_generation`,
      ),
      sceneAdmissionEpoch: integerAt(
        fences.scene_admission_epoch,
        `${path}.recovery_fences.scene_admission_epoch`,
      ),
      poseSourceApplyGeneration: integerAt(
        fences.pose_source_apply_generation,
        `${path}.recovery_fences.pose_source_apply_generation`,
      ),
    },
    audit,
  };
}

function parseCycleRecoveryAction(
  value: unknown,
  path: string,
): CycleRecoveryAction {
  const record = objectAt(value, path);
  exactKeysAt(record, [
    'action_id', 'requires_confirmation',
    'performs_backend_reset', 'enabled', 'disabled_reason_code',
  ], path);
  const actionId = stringAt(
    record.action_id,
    `${path}.action_id`,
  ) as CycleRecoveryActionId;
  if (!RECOVERY_ACTION_IDS.has(actionId)) {
    return fail(`${path}.action_id`, 'a supported recovery action');
  }
  const enabled = booleanAt(record.enabled, `${path}.enabled`);
  const disabledReasonCode = nullableStringAt(
    record.disabled_reason_code,
    `${path}.disabled_reason_code`,
  );
  if ((enabled && disabledReasonCode !== null) || (!enabled && disabledReasonCode === null)) {
    return fail(path, 'an enabled action or an explicit disabled reason');
  }
  return {
    actionId,
    requiresConfirmation: booleanAt(
      record.requires_confirmation,
      `${path}.requires_confirmation`,
    ),
    performsBackendReset: booleanAt(
      record.performs_backend_reset,
      `${path}.performs_backend_reset`,
    ),
    enabled,
    disabledReasonCode,
  };
}

function parseCycleRecoveryOffer(
  value: unknown,
  path: string,
): CycleRecoveryOffer {
  const record = objectAt(value, path);
  exactKeysAt(record, [
    'schema_version', 'offer_id', 'offer_revision', 'generated_at_ns',
    'valid_until_ns', 'kernel_assessment_revision', 'control_command_epoch',
    'operation_id', 'operation_revision', 'program_run_id',
    'program_binding_id', 'node_instance_id', 'node_attempt_id',
    'participants', 'graph_revision', 'retained_bundle_id', 'cycle_generation',
    'program_generation', 'deployment_generation', 'scene_admission_epoch',
    'pose_source_apply_generation', 'latch_id', 'stop_id', 'stop_generation',
    'stop_certificate_id', 'stop_certificate_revision', 'status',
    'recovery_class', 'actions', 'reason_code', 'message',
    'details', 'origin', 'evidence_refs', 'manual_result', 'policy',
    'retries_used',
  ], path);
  const schemaVersion = stringAt(record.schema_version, `${path}.schema_version`);
  if (schemaVersion !== 'manyforge.cycle_recovery_offer.v4') {
    return fail(`${path}.schema_version`, 'manyforge.cycle_recovery_offer.v4');
  }
  const policy = objectAt(record.policy, `${path}.policy`);
  exactKeysAt(policy, ['allow_intervention', 'max_retries'], `${path}.policy`);
  const maxRetries = integerAt(policy.max_retries, `${path}.policy.max_retries`);
  if (maxRetries < 0 || maxRetries > 255) {
    return fail(`${path}.policy.max_retries`, 'an integer in 0..255 (0 is Unlimited)');
  }
  const retriesUsed = integerAt(record.retries_used, `${path}.retries_used`);
  if (retriesUsed < 0) return fail(`${path}.retries_used`, 'a nonnegative count');
  const actions = boundedArrayAt(record.actions, `${path}.actions`, 32)
    .map((item, index) => parseCycleRecoveryAction(
      item,
      `${path}.actions[${index}]`,
    ));
  if (new Set(actions.map((action) => action.actionId)).size !== actions.length) {
    return fail(`${path}.actions`, 'unique recovery action identities');
  }
  const manualResult = objectAt(record.manual_result, `${path}.manual_result`);
  exactKeysAt(
    manualResult,
    ['enabled', 'disabled_reason_code'],
    `${path}.manual_result`,
  );
  const manualResultEnabled = booleanAt(
    manualResult.enabled,
    `${path}.manual_result.enabled`,
  );
  const manualResultDisabledReasonCode = nullableStringAt(
    manualResult.disabled_reason_code,
    `${path}.manual_result.disabled_reason_code`,
  );
  if (
    (manualResultEnabled && manualResultDisabledReasonCode !== null)
    || (!manualResultEnabled && manualResultDisabledReasonCode === null)
  ) {
    return fail(
      `${path}.manual_result`,
      'enabled availability or an explicit disabled reason',
    );
  }
  return {
    schemaVersion,
    offerId: digestAt(record.offer_id, `${path}.offer_id`),
    offerRevision: positiveIntegerAt(record.offer_revision, `${path}.offer_revision`),
    generatedAtNs: monotonicNanosecondsAt(
      record.generated_at_ns,
      `${path}.generated_at_ns`,
    ),
    validUntilNs: nullableMonotonicNanosecondsAt(
      record.valid_until_ns,
      `${path}.valid_until_ns`,
    ),
    kernelAssessmentRevision: integerAt(
      record.kernel_assessment_revision,
      `${path}.kernel_assessment_revision`,
    ),
    controlCommandEpoch: integerAt(
      record.control_command_epoch,
      `${path}.control_command_epoch`,
    ),
    operationId: nullableStringAt(record.operation_id, `${path}.operation_id`),
    operationRevision: nullablePositiveIntegerAt(
      record.operation_revision,
      `${path}.operation_revision`,
    ),
    programRunId: stringAt(record.program_run_id, `${path}.program_run_id`),
    programBindingId: digestAt(
      record.program_binding_id,
      `${path}.program_binding_id`,
    ),
    nodeInstanceId: stringAt(record.node_instance_id, `${path}.node_instance_id`),
    nodeAttemptId: stringAt(record.node_attempt_id, `${path}.node_attempt_id`),
    participants: boundedArrayAt(record.participants, `${path}.participants`, 4096)
      .map((item, index) => jsonValueAt(item, `${path}.participants[${index}]`)),
    graphRevision: digestAt(record.graph_revision, `${path}.graph_revision`),
    retainedBundleId: digestAt(
      record.retained_bundle_id,
      `${path}.retained_bundle_id`,
    ),
    cycleGeneration: positiveIntegerAt(
      record.cycle_generation,
      `${path}.cycle_generation`,
    ),
    programGeneration: integerAt(
      record.program_generation,
      `${path}.program_generation`,
    ),
    deploymentGeneration: integerAt(
      record.deployment_generation,
      `${path}.deployment_generation`,
    ),
    sceneAdmissionEpoch: integerAt(
      record.scene_admission_epoch,
      `${path}.scene_admission_epoch`,
    ),
    poseSourceApplyGeneration: integerAt(
      record.pose_source_apply_generation,
      `${path}.pose_source_apply_generation`,
    ),
    latchId: nullableStringAt(record.latch_id, `${path}.latch_id`),
    stopId: nullableStringAt(record.stop_id, `${path}.stop_id`),
    stopGeneration: nullablePositiveIntegerAt(
      record.stop_generation,
      `${path}.stop_generation`,
    ),
    stopCertificateId: nullableStringAt(
      record.stop_certificate_id,
      `${path}.stop_certificate_id`,
    ),
    stopCertificateRevision: nullablePositiveIntegerAt(
      record.stop_certificate_revision,
      `${path}.stop_certificate_revision`,
    ),
    status: stringAt(record.status, `${path}.status`),
    recoveryClass: stringAt(record.recovery_class, `${path}.recovery_class`),
    actions,
    manualResult: {
      enabled: manualResultEnabled,
      disabledReasonCode: manualResultDisabledReasonCode,
    },
    reason: {
      code: stringAt(record.reason_code, `${path}.reason_code`),
      message: failureMessageAt(record.message, `${path}.message`),
      ...failureContextAt(record, path),
    },
    evidenceRefs: stringArrayAt(record.evidence_refs, `${path}.evidence_refs`),
    policy: {
      allowIntervention: booleanAt(
        policy.allow_intervention,
        `${path}.policy.allow_intervention`,
      ),
      maxRetries,
    },
    retriesUsed,
  };
}

function parseExecutionSelectionControls(value: unknown, path: string): ProgramExecutionSelectionControls {
  const record = objectAt(value, path);
  exactKeysAt(record, ['selection_revision', 'motion_held', 'groups'], path);
  const selectionRevision = integerAt(record.selection_revision, `${path}.selection_revision`);
  if (selectionRevision < 0) return fail(path, 'a nonnegative selection revision');
  const motionHeld = booleanAt(record.motion_held, `${path}.motion_held`);
  let previous = '';
  const groups = boundedArrayAt(record.groups, `${path}.groups`, 4097).map((raw, index) => {
    const key = `${path}.groups[${index}]`;
    const row = objectAt(raw, key);
    exactKeysAt(row, ['group_id', 'node_count', 'held_node_count', 'state',
      'stop_allowed', 'start_allowed', 'start_unavailable_reason',
      'resume_allowed', 'resume_unavailable_reason',
      'runtime_scale_selector', 'runtime_scale_unavailable_reason',
      'resource_ids'], key);
    const groupId = stringAt(row.group_id, `${key}.group_id`);
    if (!/^[a-z][a-z0-9_.-]{0,127}$/.test(groupId) || groupId === 'inherit' || groupId <= previous) {
      return fail(key, 'canonical unique captured group IDs');
    }
    previous = groupId;
    const nodeCount = positiveIntegerAt(row.node_count, `${key}.node_count`);
    const heldNodeCount = integerAt(row.held_node_count, `${key}.held_node_count`);
    const state = stringAt(row.state, `${key}.state`);
    if (!['ready', 'running', 'held', 'mixed', 'stopping', 'faulted', 'unavailable'].includes(state)) {
      return fail(key, 'an owner selection state');
    }
    const stopAllowed = booleanAt(row.stop_allowed, `${key}.stop_allowed`);
    const startAllowed = booleanAt(row.start_allowed, `${key}.start_allowed`);
    const startUnavailableReason = nullableStringAt(
      row.start_unavailable_reason,
      `${key}.start_unavailable_reason`,
    );
    const resumeAllowed = booleanAt(row.resume_allowed, `${key}.resume_allowed`);
    const resumeUnavailableReason = nullableStringAt(row.resume_unavailable_reason, `${key}.resume_unavailable_reason`);
    const runtimeScaleSelector = row.runtime_scale_selector === null
      ? null
      : runtimeScaleControlSelectorAt(
        row.runtime_scale_selector,
        `${key}.runtime_scale_selector`,
      );
    const runtimeScaleUnavailableReason = nullableStringAt(
      row.runtime_scale_unavailable_reason,
      `${key}.runtime_scale_unavailable_reason`,
    );
    if ((runtimeScaleSelector === null)
      !== (runtimeScaleUnavailableReason !== null)) {
      return fail(
        key,
        'a complete runtime-scale selector or an owner unavailability reason',
      );
    }
    const resourceIds = boundedArrayAt(row.resource_ids, `${key}.resource_ids`, 4096)
      .map((item, resourceIndex) => stringAt(item, `${key}.resource_ids[${resourceIndex}]`));
    if (resourceIds.some((resource, resourceIndex) => (
      resource.length === 0 || (resourceIndex > 0 && resource <= resourceIds[resourceIndex - 1])
    ))) {
      return fail(`${key}.resource_ids`, 'sorted unique resource IDs');
    }
    if (nodeCount > 100_000 || heldNodeCount < 0 || heldNodeCount > nodeCount
      || (state === 'running' && heldNodeCount !== 0)
      || (state === 'held' && heldNodeCount !== nodeCount)
      // Ready and stopped nodes may be mixed even when every node is held.
      || (state === 'mixed' && heldNodeCount === 0)
      // Run and Resume are one release of never-started and stopped work:
      // the owner projects one eligibility for both.
      || startAllowed !== resumeAllowed
      || (startAllowed && !['ready', 'held', 'mixed'].includes(state))
      || startAllowed !== (startUnavailableReason === null)
      || resumeAllowed !== (resumeUnavailableReason === null)) {
      return fail(key, 'coherent bounded selection status and eligibility');
    }
    return { groupId, nodeCount, heldNodeCount,
      state: state as ProgramExecutionSelectionControls['groups'][number]['state'],
      stopAllowed, startAllowed, startUnavailableReason,
      resumeAllowed, resumeUnavailableReason,
      runtimeScaleSelector, runtimeScaleUnavailableReason, resourceIds };
  });
  if (!groups.some((group) => group.groupId === 'all')) return fail(path, 'the built-in all selection');
  return { selectionRevision, motionHeld, groups };
}

function parseProgramRecoveryHold(value: unknown, path: string): ProgramRecoveryHold {
  const record = objectAt(value, path);
  exactKeysAt(record, ['motion_group_ids', 'intervention_stop_scope', 'offer'], path);
  const motionGroupIds = boundedArrayAt(record.motion_group_ids, `${path}.motion_group_ids`, 4097)
    .map((item, index) => nonEmptyStringAt(item, `${path}.motion_group_ids[${index}]`));
  if (motionGroupIds.length === 0 || new Set(motionGroupIds).size !== motionGroupIds.length) {
    return fail(`${path}.motion_group_ids`, 'at least one unique motion group ID');
  }
  const scope = stringAt(record.intervention_stop_scope, `${path}.intervention_stop_scope`);
  if (scope !== 'all' && scope !== 'own_groups') {
    return fail(`${path}.intervention_stop_scope`, 'all or own_groups');
  }
  return {
    motionGroupIds,
    interventionStopScope: scope,
    offer: parseCycleRecoveryOffer(record.offer, `${path}.offer`),
  };
}

export function parseProgramRuntimeControlProjection(
  value: unknown,
): ProgramRuntimeControlProjection {
  const path = 'programRuntimeControlProjection';
  const record = objectAt(value, path);
  exactKeysAt(record, [
    'schema_version', 'owner_id', 'owner_epoch', 'program_run_id',
    'run_revision', 'runtime_controls', 'recovery_offer', 'recovery_offers',
    'execution_selections',
  ], path);
  const schemaVersion = stringAt(record.schema_version, `${path}.schema_version`);
  if (schemaVersion !== 'manyforge.program_runtime_control_projection.v9') {
    return fail(
      `${path}.schema_version`,
      'manyforge.program_runtime_control_projection.v9',
    );
  }
  const programRunId = stringAt(record.program_run_id, `${path}.program_run_id`);
  const runtimeControls = parseProgramRuntimeControlsSnapshot(
    record.runtime_controls,
    `${path}.runtime_controls`,
  );
  const recoveryOffer = record.recovery_offer === null
    ? null
    : parseCycleRecoveryOffer(record.recovery_offer, `${path}.recovery_offer`);
  const recoveryHolds = boundedArrayAt(record.recovery_offers, `${path}.recovery_offers`, 4096)
    .map((item, index) => parseProgramRecoveryHold(item, `${path}.recovery_offers[${index}]`));
  if (
    runtimeControls.programRunId !== programRunId
    || (recoveryOffer !== null && recoveryOffer.programRunId !== programRunId)
    || recoveryHolds.some((hold) => hold.offer.programRunId !== programRunId)
  ) {
    return fail(path, 'one exact Program-run identity');
  }
  if (
    new Set(recoveryHolds.map((hold) => hold.offer.offerId)).size !== recoveryHolds.length
    || new Set(recoveryHolds.map((hold) => hold.offer.nodeInstanceId)).size !== recoveryHolds.length
  ) {
    return fail(`${path}.recovery_offers`, 'one offer per held node');
  }
  return {
    schemaVersion,
    ownerId: stringAt(record.owner_id, `${path}.owner_id`),
    ownerEpoch: stringAt(record.owner_epoch, `${path}.owner_epoch`),
    programRunId,
    runRevision: positiveIntegerAt(record.run_revision, `${path}.run_revision`),
    runtimeControls,
    recoveryOffer,
    recoveryHolds,
    executionSelections: parseExecutionSelectionControls(record.execution_selections, `${path}.execution_selections`),
  };
}

function persistentKeyValueAt(value: unknown, path: string): PersistentKeyValue {
  const record = objectAt(value, path);
  exactKeysAt(record, [
    'access', 'has_initial', 'initial', 'key', 'sequence', 'stored', 'type', 'value',
  ], path);
  const access = stringAt(record.access, `${path}.access`);
  if (access !== 'read' && access !== 'write' && access !== 'read_write') {
    return fail(`${path}.access`, 'a supported access mode');
  }
  const hasInitial = booleanAt(record.has_initial, `${path}.has_initial`);
  const initial = jsonValueAt(record.initial, `${path}.initial`);
  if (hasInitial === (initial === null)) {
    return fail(`${path}.initial`, 'a value exactly when has_initial is true');
  }
  const stored = booleanAt(record.stored, `${path}.stored`);
  if (!stored && (record.value !== null || record.sequence !== null)) {
    return fail(path, 'without a value or sequence when nothing is stored');
  }
  return {
    key: stringAt(record.key, `${path}.key`),
    type: stringAt(record.type, `${path}.type`),
    access,
    initial: initial === null ? undefined : initial,
    stored,
    value: jsonValueAt(record.value, `${path}.value`),
    sequence: stored ? positiveIntegerAt(record.sequence, `${path}.sequence`) : null,
  };
}

function persistentKeyValueEditAt(value: unknown, path: string): PersistentKeyValueEdit {
  const record = objectAt(value, path);
  exactKeysAt(record, [
    'action', 'detail', 'key', 'node_attempt_id', 'node_instance_id', 'program_run_id',
    'reason', 'request_id', 'scenario_id', 'sequence', 'source_label',
  ], path);
  const action = stringAt(record.action, `${path}.action`);
  if (
    action !== 'set' && action !== 'reset' && action !== 'clear'
    && action !== 'dropped' && action !== 'fault'
  ) {
    return fail(`${path}.action`, 'set, reset, clear, dropped or fault');
  }
  const reason = nullableStringAt(record.reason, `${path}.reason`);
  if ((reason !== null) !== (action === 'dropped' || action === 'fault')) {
    return fail(`${path}.reason`, 'a reason exactly for a dropped value or a fault');
  }
  return {
    sequence: positiveIntegerAt(record.sequence, `${path}.sequence`),
    key: nullableStringAt(record.key, `${path}.key`),
    action,
    requestId: nullableStringAt(record.request_id, `${path}.request_id`),
    sourceLabel: stringAt(record.source_label, `${path}.source_label`),
    programRunId: nullableStringAt(record.program_run_id, `${path}.program_run_id`),
    nodeInstanceId: nullableStringAt(record.node_instance_id, `${path}.node_instance_id`),
    nodeAttemptId: nullableStringAt(record.node_attempt_id, `${path}.node_attempt_id`),
    reason,
    detail: nullableStringAt(record.detail, `${path}.detail`),
  };
}

function persistentKeyStorageFaultAt(
  value: unknown,
  path: string,
): PersistentKeyValues['storageFault'] {
  if (value === null) return null;
  const record = objectAt(value, path);
  exactKeysAt(record, ['code', 'message'], path);
  return {
    code: stringAt(record.code, `${path}.code`),
    message: stringAt(record.message, `${path}.message`),
  };
}

function persistentKeyApplyObservationFaultAt(
  value: unknown,
  path: string,
): PersistentKeyValues['applyObservationFault'] {
  if (value === null) return null;
  const record = objectAt(value, path);
  exactKeysAt(record, ['code', 'record_digests'], path);
  const recordDigests = arrayAt(record.record_digests, `${path}.record_digests`)
    .map((digest, index) => digestAt(digest, `${path}.record_digests[${index}]`));
  if (recordDigests.length === 0) {
    return fail(`${path}.record_digests`, 'at least one committed Apply');
  }
  return { code: stringAt(record.code, `${path}.code`), recordDigests };
}

export function parsePersistentKeyValues(
  value: unknown,
  path = 'persistentKeyValues',
): PersistentKeyValues {
  const record = objectAt(value, path);
  exactKeysAt(record, [
    'apply_observation_fault', 'edits', 'keys', 'program_revision', 'program_run_id',
    'scenario_id', 'schema_version', 'sequence', 'storage_fault',
  ], path);
  if (record.schema_version !== 'manyforge.persistent_key_values.v1') {
    return fail(`${path}.schema_version`, 'manyforge.persistent_key_values.v1');
  }
  return {
    scenarioId: stringAt(record.scenario_id, `${path}.scenario_id`),
    programRevision: digestAt(record.program_revision, `${path}.program_revision`),
    programRunId: nullableStringAt(record.program_run_id, `${path}.program_run_id`),
    sequence: integerAt(record.sequence, `${path}.sequence`),
    keys: arrayAt(record.keys, `${path}.keys`)
      .map((row, index) => persistentKeyValueAt(row, `${path}.keys[${index}]`)),
    edits: arrayAt(record.edits, `${path}.edits`)
      .map((row, index) => persistentKeyValueEditAt(row, `${path}.edits[${index}]`)),
    storageFault: persistentKeyStorageFaultAt(record.storage_fault, `${path}.storage_fault`),
    applyObservationFault: persistentKeyApplyObservationFaultAt(
      record.apply_observation_fault,
      `${path}.apply_observation_fault`,
    ),
  };
}

/** A set or Reset returns the Scenario's values as they are after the edit. */
export function parsePersistentKeyEdit(value: unknown): PersistentKeyValues {
  const path = 'persistentKeyEdit';
  const record = objectAt(value, path);
  exactKeysAt(record, ['action', 'key', 'request_id', 'schema_version', 'values'], path);
  if (record.schema_version !== 'manyforge.persistent_key_edit.v1') {
    return fail(`${path}.schema_version`, 'manyforge.persistent_key_edit.v1');
  }
  if (record.action !== 'set' && record.action !== 'reset') {
    return fail(`${path}.action`, 'set or reset');
  }
  return parsePersistentKeyValues(record.values, `${path}.values`);
}

export function parsePersistentKeyValuesCleared(value: unknown): PersistentKeyValuesCleared {
  const path = 'persistentKeyValuesCleared';
  const record = objectAt(value, path);
  exactKeysAt(record, ['action', 'removed', 'request_id', 'scenario_id', 'schema_version'], path);
  if (record.schema_version !== 'manyforge.persistent_key_edit.v1' || record.action !== 'clear') {
    return fail(path, 'a manyforge.persistent_key_edit.v1 clear result');
  }
  return {
    requestId: stringAt(record.request_id, `${path}.request_id`),
    scenarioId: stringAt(record.scenario_id, `${path}.scenario_id`),
    removed: integerAt(record.removed, `${path}.removed`),
  };
}

const EXPRESSION_VALUE_TYPES: ReadonlySet<string> = new Set(['bool', 'int64', 'float64', 'string']);

function expressionValueTypeAt(value: unknown, path: string): ExpressionValueType {
  const type = stringAt(value, path);
  if (!EXPRESSION_VALUE_TYPES.has(type)) return fail(path, 'bool, int64, float64 or string');
  return type as ExpressionValueType;
}

function expressionDiagnosticAt(value: unknown, path: string): ExpressionDiagnostic {
  const record = objectAt(value, path);
  exactKeysAt(record, ['code', 'length', 'message', 'name', 'position'], path);
  const position = record.position === null ? null : integerAt(record.position, `${path}.position`);
  const length = record.length === null ? null : integerAt(record.length, `${path}.length`);
  if ((position === null) !== (length === null)) {
    return fail(path, 'a position and a length together, or neither');
  }
  return {
    code: nonEmptyStringAt(record.code, `${path}.code`),
    message: nonEmptyStringAt(record.message, `${path}.message`),
    position,
    length,
    name: nullableStringAt(record.name, `${path}.name`),
  };
}

function expressionPreviewMatches(type: ExpressionValueType, value: JsonValue): boolean {
  if (type === 'bool') return typeof value === 'boolean';
  if (type === 'string') return typeof value === 'string';
  if (type === 'int64') return typeof value === 'number' && Number.isInteger(value);
  return typeof value === 'number' && Number.isFinite(value);
}

/** The backend's check of one formula (POST /api/program/draft/expression-check). */
export function parseExpressionCheckResult(value: unknown): ExpressionCheckResult {
  const path = 'expressionCheck';
  const record = objectAt(value, path);
  exactKeysAt(record, ['diagnostics', 'names', 'preview', 'result_type', 'schema_version'], path);
  if (record.schema_version !== 'manyforge.expression_check.v1') {
    return fail(`${path}.schema_version`, 'manyforge.expression_check.v1');
  }
  const resultType = record.result_type === null
    ? null
    : expressionValueTypeAt(record.result_type, `${path}.result_type`);
  const diagnostics = arrayAt(record.diagnostics, `${path}.diagnostics`)
    .map((row, index) => expressionDiagnosticAt(row, `${path}.diagnostics[${index}]`));
  const previewRecord = objectAt(record.preview, `${path}.preview`);
  exactKeysAt(previewRecord, ['unavailable_reason', 'value'], `${path}.preview`);
  const previewValue = jsonValueAt(previewRecord.value, `${path}.preview.value`);
  const reason = nullableStringAt(
    previewRecord.unavailable_reason,
    `${path}.preview.unavailable_reason`,
  );
  let preview: ExpressionCheckResult['preview'];
  if (previewValue !== null && reason === null) {
    if (resultType === null || !expressionPreviewMatches(resultType, previewValue)) {
      return fail(`${path}.preview.value`, 'a value of the result type');
    }
    preview = { value: previewValue };
  } else if (previewValue === null && reason !== null && reason !== '') {
    preview = { unavailableReason: reason };
  } else {
    return fail(`${path}.preview`, 'either a value or a non-empty unavailable_reason');
  }
  const names = arrayAt(record.names, `${path}.names`).map((row, index) => {
    const namePath = `${path}.names[${index}]`;
    const name = objectAt(row, namePath);
    exactKeysAt(name, ['name', 'type'], namePath);
    const text = stringAt(name.name, `${namePath}.name`);
    if (!/^(param|key)\.\S+$/.test(text)) return fail(`${namePath}.name`, 'a param. or key. name');
    return { name: text, type: nonEmptyStringAt(name.type, `${namePath}.type`) };
  });
  return { resultType, diagnostics, preview, names };
}

function parseImplementationAvailability(
  value: unknown,
  path: string,
): NodeImplementationAvailability {
  const record = objectAt(value, path);
  exactKeysAt(
    record,
    [
      'adapter_build_qualified',
      'authoring_available',
      'binding_available',
      'engine_id',
      'executable_package_id',
      'execution_available',
      'implementation_id',
      'implementation_identity_resolved',
      'node_id',
      'owner_health_available',
      'reason_codes',
      'restoration_gates',
    ],
    path,
  );
  for (const field of [
    'adapter_build_qualified',
    'binding_available',
    'implementation_identity_resolved',
    'owner_health_available',
  ]) {
    booleanAt(record[field], `${path}.${field}`);
  }
  return {
    nodeId: stringAt(record.node_id, `${path}.node_id`),
    engineId: stringAt(record.engine_id, `${path}.engine_id`),
    implementationId: stringAt(record.implementation_id, `${path}.implementation_id`),
    executablePackageId: stringAt(
      record.executable_package_id,
      `${path}.executable_package_id`,
    ),
    authoringAvailable: booleanAt(record.authoring_available, `${path}.authoring_available`),
    executionAvailable: booleanAt(record.execution_available, `${path}.execution_available`),
    reasonCodes: stringArrayAt(record.reason_codes, `${path}.reason_codes`),
    restorationGates: stringArrayAt(record.restoration_gates, `${path}.restoration_gates`),
  };
}

export function parseNodeEnvironmentResponse(value: unknown): NodeEnvironmentResponse {
  const record = objectAt(value, 'nodeEnvironment');
  allowedKeysAt(
    record,
    [
      'environmentDigest',
      'catalog',
      'executableRegistry',
      'executablePackages',
      'sourceCoverage',
    ],
    ['availability', 'active', 'source'],
    'nodeEnvironment',
  );
  arrayAt(record.executablePackages, 'nodeEnvironment.executablePackages');
  arrayAt(record.sourceCoverage, 'nodeEnvironment.sourceCoverage');
  objectAt(record.catalog, 'nodeEnvironment.catalog');
  const registry = arrayAt(record.executableRegistry, 'nodeEnvironment.executableRegistry');
  const fallbackImplementations = registry.map((item, index) => {
    const path = `nodeEnvironment.executableRegistry[${index}]`;
    const row = objectAt(item, path);
    exactKeysAt(
      row,
      ['node_id', 'engine_id', 'implementation_id', 'executable_package_id'],
      path,
    );
    return {
      nodeId: stringAt(row.node_id, `${path}.node_id`),
      engineId: stringAt(row.engine_id, `${path}.engine_id`),
      implementationId: stringAt(row.implementation_id, `${path}.implementation_id`),
      executablePackageId: stringAt(
        row.executable_package_id,
        `${path}.executable_package_id`,
      ),
      authoringAvailable: true,
      executionAvailable: false,
      reasonCodes: ['active_node_environment_unavailable'],
      restorationGates: [],
    };
  });
  if (record.availability === undefined) {
    return {
      environmentDigest: stringAt(record.environmentDigest, 'nodeEnvironment.environmentDigest'),
      implementations: fallbackImplementations,
      portableAuthoringAvailable: true,
      executionAvailable: false,
      reasonCodes: ['active_node_environment_unavailable'],
      restorationGates: [],
      active: null,
    };
  }
  const availability = objectAt(record.availability, 'nodeEnvironment.availability');
  exactKeysAt(
    availability,
    [
      'binding_context',
      'execution_available',
      'implementations',
      'manager_runtime_converged',
      'portable_authoring_available',
      'reason_codes',
      'restoration_gates',
    ],
    'nodeEnvironment.availability',
  );
  const active = objectAt(record.active, 'nodeEnvironment.active');
  exactKeysAt(active, ['retainedBundleId', 'graphRevision'], 'nodeEnvironment.active');
  return {
    environmentDigest: stringAt(record.environmentDigest, 'nodeEnvironment.environmentDigest'),
    implementations: arrayAt(
      availability.implementations,
      'nodeEnvironment.availability.implementations',
    ).map((item, index) => parseImplementationAvailability(
      item,
      `nodeEnvironment.availability.implementations[${index}]`,
    )),
    portableAuthoringAvailable: booleanAt(
      availability.portable_authoring_available,
      'nodeEnvironment.availability.portable_authoring_available',
    ),
    executionAvailable: booleanAt(
      availability.execution_available,
      'nodeEnvironment.availability.execution_available',
    ),
    reasonCodes: stringArrayAt(
      availability.reason_codes,
      'nodeEnvironment.availability.reason_codes',
    ),
    restorationGates: stringArrayAt(
      availability.restoration_gates,
      'nodeEnvironment.availability.restoration_gates',
    ),
    active: {
      retainedBundleId: stringAt(
        active.retainedBundleId,
        'nodeEnvironment.active.retainedBundleId',
      ),
      graphRevision: stringAt(
        active.graphRevision,
        'nodeEnvironment.active.graphRevision',
      ),
    },
  };
}

const RUNTIME_SCALE_STATES = new Set([
  'pending',
  'applied',
  'rejected',
  'failed',
]);

function runtimeScalePortableAt(value: unknown, field: string): string {
  const result = stringAt(value, field);
  if (result.length === 0 || result !== result.normalize('NFC')
    || new TextEncoder().encode(result).length > 256) {
    return fail(field, 'a non-empty NFC-normalized identifier within 256 UTF-8 bytes');
  }
  return result;
}

function runtimeScaleControlSelectorAt(
  value: unknown,
  path: string,
): RuntimeScaleControlStatus['selector'] {
  const selector = objectAt(value, path);
  const binding = runtimeScalePortableAt(selector.program_binding_id, `${path}.program_binding_id`);
  if (Object.prototype.hasOwnProperty.call(selector, 'group_targets')) {
    exactKeysAt(selector, ['group_targets', 'program_binding_id'], path);
    const rows = arrayAt(selector.group_targets, `${path}.group_targets`);
    if (rows.length < 2 || rows.length > 4096) {
      return fail(`${path}.group_targets`, 'between 2 and 4096 targets');
    }
    const targets = rows.map((row, index) => {
      const member = objectAt(row, `${path}.group_targets[${index}]`);
      exactKeysAt(member, ['component_role_id', 'target_role_id'], `${path}.group_targets[${index}]`);
      return {
        componentRoleId: runtimeScalePortableAt(member.component_role_id, `${path}.group_targets[${index}].component_role_id`),
        targetRoleId: runtimeScalePortableAt(member.target_role_id, `${path}.group_targets[${index}].target_role_id`),
      };
    });
    const identities = targets.map((target) => runtimeScalePairIdentity(
      target.componentRoleId,
      target.targetRoleId,
    ));
    if (new Set(identities).size !== identities.length) {
      return fail(`${path}.group_targets`, 'unique target pairs');
    }
    return { kind: 'group', programBindingId: binding, groupTargets: targets };
  }
  exactKeysAt(selector, ['component_role_id', 'program_binding_id', 'target_role_id'], path);
  return {
    kind: 'singular',
    componentRoleId: runtimeScalePortableAt(selector.component_role_id, `${path}.component_role_id`),
    programBindingId: binding,
    targetRoleId: runtimeScalePortableAt(selector.target_role_id, `${path}.target_role_id`),
  };
}

function nullableNonnegativeIntegerAt(
  value: unknown,
  path: string,
): number | null {
  if (value === null) return null;
  const result = integerAt(value, path);
  return result >= 0 ? result : fail(path, 'a nonnegative integer or null');
}

function nonnegativeIntegerAt(value: unknown, path: string): number {
  const result = nullableNonnegativeIntegerAt(value, path);
  return result === null ? fail(path, 'a nonnegative integer') : result;
}

export function runtimeScaleControlStatusAt(
  value: unknown,
  path: string,
): RuntimeScaleControlStatus {
  const status = objectAt(value, path);
  const selector = runtimeScaleControlSelectorAt(status.selector, `${path}.selector`);
  exactKeysAt(status, [
    'active_control_configuration_generation',
    'active_control_configuration_id',
    'active_operation_id',
    'active_operation_revision',
    'active_operation_scale_generation',
    'application_state',
    'control_binding_id',
    'control_binding_revision',
    'control_domain_id',
    'control_domain_revision',
    'control_generation',
    'effective_scale',
    'frozen',
    'reason',
    'requested_scale',
    'scaling_domain_id',
    'selector',
    'supported',
    ...(selector.kind === 'group' ? ['group_members'] : []),
  ], path);
  const controlGeneration = integerAt(
    status.control_generation,
    `${path}.control_generation`,
  );
  const requestedScale = numberAt(status.requested_scale, `${path}.requested_scale`);
  const effectiveScale = nullableNumberAt(status.effective_scale, `${path}.effective_scale`);
  if (
    controlGeneration < 0
    || requestedScale < 0 || requestedScale > 1
    || (effectiveScale !== null && (effectiveScale < 0 || effectiveScale > 1))
  ) {
    return fail(path, 'nonnegative generations and scales within [0, 1]');
  }
  const applicationState = nullableStringAt(
    status.application_state,
    `${path}.application_state`,
  );
  if (applicationState !== null && !RUNTIME_SCALE_STATES.has(applicationState)) {
    return fail(`${path}.application_state`, 'a known runtime-scale state or null');
  }
  let reason: string | null = null;
  if (status.reason !== null) {
    const error = objectAt(status.reason, `${path}.reason`);
    exactKeysAt(error, ['code', 'details', 'message'], `${path}.reason`);
    objectAt(error.details, `${path}.reason.details`);
    stringAt(error.code, `${path}.reason.code`);
    reason = stringAt(error.message, `${path}.reason.message`);
  }
  const controlDomainId = nullableStringAt(
    status.control_domain_id,
    `${path}.control_domain_id`,
  );
  const controlDomainRevision = nullableStringAt(
    status.control_domain_revision,
    `${path}.control_domain_revision`,
  );
  const controlBindingId = nullableStringAt(
    status.control_binding_id,
    `${path}.control_binding_id`,
  );
  const controlBindingRevision = nullableStringAt(
    status.control_binding_revision,
    `${path}.control_binding_revision`,
  );
  const scalingDomainId = nullableStringAt(
    status.scaling_domain_id,
    `${path}.scaling_domain_id`,
  );
  const activeControlConfigurationId = nullableStringAt(
    status.active_control_configuration_id,
    `${path}.active_control_configuration_id`,
  );
  const activeControlConfigurationGeneration = nullableNonnegativeIntegerAt(
    status.active_control_configuration_generation,
    `${path}.active_control_configuration_generation`,
  );
  const resolvedScope = [
    controlDomainId,
    controlDomainRevision,
    controlBindingId,
    controlBindingRevision,
    scalingDomainId,
    activeControlConfigurationId,
    activeControlConfigurationGeneration,
  ];
  const supported = booleanAt(status.supported, `${path}.supported`);
  if (selector.kind === 'singular' && supported !== resolvedScope.every((item) => item !== null)) {
    return fail(path, 'support truth consistent with one complete resolved control scope');
  }
  let groupMembers: RuntimeScaleControlStatus['groupMembers'];
  if (selector.kind === 'group') {
    const rows = arrayAt(status.group_members, `${path}.group_members`);
    if (rows.length < 2 || rows.length > 4096) {
      return fail(`${path}.group_members`, 'between 2 and 4096 members');
    }
    const expected = new Set(selector.groupTargets.map((target) => (
      runtimeScalePairIdentity(target.componentRoleId, target.targetRoleId)
    )));
    const seen = new Set<string>();
    if (!supported) return fail(path, 'a group must be supported');
    groupMembers = rows.map((row, index) => {
      const member = objectAt(row, `${path}.group_members[${index}]`);
      exactKeysAt(member, [
        'active_control_configuration_generation', 'active_control_configuration_id',
        'control_binding_id', 'control_binding_revision', 'control_domain_id',
        'control_domain_revision', 'scaling_domain_id', 'component_role_id', 'target_role_id',
      ], `${path}.group_members[${index}]`);
      const componentRoleId = runtimeScalePortableAt(member.component_role_id, `${path}.group_members[${index}].component_role_id`);
      const targetRoleId = runtimeScalePortableAt(member.target_role_id, `${path}.group_members[${index}].target_role_id`);
      const identity = runtimeScalePairIdentity(componentRoleId, targetRoleId);
      if (!expected.has(identity) || seen.has(identity)) return fail(`${path}.group_members[${index}]`, 'exactly one selected target');
      seen.add(identity);
      const fields = [
        'control_domain_id', 'control_domain_revision', 'control_binding_id',
        'control_binding_revision', 'scaling_domain_id', 'active_control_configuration_id',
      ].map((field) => runtimeScalePortableAt(member[field], `${path}.group_members[${index}].${field}`));
      return {
        componentRoleId, targetRoleId,
        controlDomainId: fields[0], controlDomainRevision: fields[1], controlBindingId: fields[2],
        controlBindingRevision: fields[3], scalingDomainId: fields[4], activeControlConfigurationId: fields[5],
        activeControlConfigurationGeneration: nonnegativeIntegerAt(
          member.active_control_configuration_generation,
          `${path}.group_members[${index}].active_control_configuration_generation`,
        ),
      };
    });
    if (seen.size !== expected.size || resolvedScope.some((item) => item !== null)) {
      return fail(path, 'group members must exactly resolve the selected scope while group scope stays null');
    }
  }
  return {
    selector,
    controlGeneration,
    requestedScale,
    supported,
    frozen: booleanAt(status.frozen, `${path}.frozen`),
    controlDomainId,
    controlDomainRevision,
    controlBindingId,
    controlBindingRevision,
    scalingDomainId,
    activeControlConfigurationId,
    activeControlConfigurationGeneration,
    activeOperationId: nullableStringAt(
      status.active_operation_id,
      `${path}.active_operation_id`,
    ),
    activeOperationRevision: nullableNonnegativeIntegerAt(
      status.active_operation_revision,
      `${path}.active_operation_revision`,
    ),
    activeOperationScaleGeneration: nullableNonnegativeIntegerAt(
      status.active_operation_scale_generation,
      `${path}.active_operation_scale_generation`,
    ),
    effectiveScale,
    applicationState: applicationState as RuntimeScaleControlStatus['applicationState'],
    reason,
    ...(groupMembers === undefined ? {} : { groupMembers }),
  };
}

export function parseRuntimeScaleControlStatus(
  value: unknown,
): RuntimeScaleControlStatus {
  const envelope = objectAt(value, 'runtimeScaleControl');
  exactKeysAt(
    envelope,
    ['api_version', 'command', 'ok', 'runtime_scale_control'],
    'runtimeScaleControl',
  );
  if (
    booleanAt(envelope.ok, 'runtimeScaleControl.ok') !== true
    || stringAt(envelope.command, 'runtimeScaleControl.command')
      !== 'get_runtime_scale_control'
  ) {
    return fail('runtimeScaleControl', 'a successful control-scope response');
  }
  return runtimeScaleControlStatusAt(
    envelope.runtime_scale_control,
    'runtimeScaleControl.runtime_scale_control',
  );
}

export function parseRuntimeScaleControlAccepted(
  value: unknown,
): RuntimeScaleControlAccepted {
  const envelope = objectAt(value, 'runtimeScaleControlAcceptance');
  exactKeysAt(
    envelope,
    ['accepted', 'api_version', 'command', 'ok'],
    'runtimeScaleControlAcceptance',
  );
  if (
    booleanAt(envelope.ok, 'runtimeScaleControlAcceptance.ok') !== true
    || stringAt(envelope.command, 'runtimeScaleControlAcceptance.command')
      !== 'set_runtime_scale_control'
  ) {
    return fail(
      'runtimeScaleControlAcceptance',
      'a successful control-scope acceptance',
    );
  }
  const accepted = objectAt(
    envelope.accepted,
    'runtimeScaleControlAcceptance.accepted',
  );
  exactKeysAt(accepted, [
    'accepted_at_ns',
    'active_operation_id',
    'active_operation_revision',
    'active_operation_scale_generation',
    'control_generation',
    'replay_status',
    'request_id',
    'requested_scale',
    'selector',
  ], 'runtimeScaleControlAcceptance.accepted');
  const controlGeneration = integerAt(
    accepted.control_generation,
    'runtimeScaleControlAcceptance.accepted.control_generation',
  );
  const requestedScale = numberAt(
    accepted.requested_scale,
    'runtimeScaleControlAcceptance.accepted.requested_scale',
  );
  if (controlGeneration < 1 || requestedScale < 0 || requestedScale > 1) {
    return fail(
      'runtimeScaleControlAcceptance.accepted',
      'a positive generation and scale within [0, 1]',
    );
  }
  stringAt(
    accepted.replay_status,
    'runtimeScaleControlAcceptance.accepted.replay_status',
  );
  monotonicNanosecondsAt(
    accepted.accepted_at_ns,
    'runtimeScaleControlAcceptance.accepted.accepted_at_ns',
  );
  return {
    requestId: stringAt(
      accepted.request_id,
      'runtimeScaleControlAcceptance.accepted.request_id',
    ),
    selector: runtimeScaleControlSelectorAt(
      accepted.selector,
      'runtimeScaleControlAcceptance.accepted.selector',
    ),
    requestedScale,
    controlGeneration,
    activeOperationId: nullableStringAt(
      accepted.active_operation_id,
      'runtimeScaleControlAcceptance.accepted.active_operation_id',
    ),
    activeOperationRevision: nullableNonnegativeIntegerAt(
      accepted.active_operation_revision,
      'runtimeScaleControlAcceptance.accepted.active_operation_revision',
    ),
    activeOperationScaleGeneration: nullableNonnegativeIntegerAt(
      accepted.active_operation_scale_generation,
      'runtimeScaleControlAcceptance.accepted.active_operation_scale_generation',
    ),
  };
}

export function parseProgramRunAdmittedSpeeds(value: unknown): ProgramRunAdmittedSpeeds {
  const path = 'programRunAdmittedSpeeds';
  const root = objectAt(value, path);
  exactKeysAt(root, ['schema_version', 'program_run_id', 'targets'], path);
  if (root.schema_version !== 'manyforge.program_run_admitted_speeds.v1') {
    return fail(`${path}.schema_version`, 'manyforge.program_run_admitted_speeds.v1');
  }
  const targets = arrayAt(root.targets, `${path}.targets`).map((item, index) => {
    const at = `${path}.targets[${index}]`;
    const row = objectAt(item, at);
    exactKeysAt(row, [
      'controllable_component_id', 'command_target_id', 'operation_id', 'occurred_at_wall',
      'requested_tcp_linear_speed_mps', 'deployment_maximum_tcp_linear_speed_mps',
      'admitted_tcp_linear_speed_mps',
    ], at);
    const admitted = numberAt(row.admitted_tcp_linear_speed_mps, `${at}.admitted_tcp_linear_speed_mps`);
    if (admitted <= 0) return fail(`${at}.admitted_tcp_linear_speed_mps`, 'positive');
    return {
      controllableComponentId: stringAt(row.controllable_component_id, `${at}.controllable_component_id`),
      commandTargetId: stringAt(row.command_target_id, `${at}.command_target_id`),
      operationId: stringAt(row.operation_id, `${at}.operation_id`),
      occurredAtWall: stringAt(row.occurred_at_wall, `${at}.occurred_at_wall`),
      requestedTcpLinearSpeedMps: row.requested_tcp_linear_speed_mps === null
        ? null
        : numberAt(row.requested_tcp_linear_speed_mps, `${at}.requested_tcp_linear_speed_mps`),
      deploymentMaximumTcpLinearSpeedMps: numberAt(
        row.deployment_maximum_tcp_linear_speed_mps,
        `${at}.deployment_maximum_tcp_linear_speed_mps`,
      ),
      admittedTcpLinearSpeedMps: admitted,
    };
  });
  return { programRunId: stringAt(root.program_run_id, `${path}.program_run_id`), targets };
}
