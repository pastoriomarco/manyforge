// Parsers for the component model v2 wire types (types/components.ts).
// They check shape only; every value is the backend's (R4).
import {
  ACTION_OUTCOMES,
  APPLY_DISPOSITIONS,
  COMPONENT_ACTIVITIES,
  COMPONENT_LIFECYCLES,
  FAULT_OPERATOR_ACTIONS,
  LATCH_CAUSE_KINDS,
  RUN_END_REASONS,
  SIGNAL_SOURCES,
  STREAM_FRESHNESS,
  type ActionError,
  type ActionResult,
  type ApplyPreview,
  type ApplyResult,
  type CellLatch,
  type CodedError,
  type ComponentState,
  type EffectiveValues,
  type Fault,
  type ProgramRunEnd,
  type RunBlockedReason,
  type RunReadiness,
  type SetOutputResult,
  type SignalSample,
} from '../../types/components';
import {
  arrayAt,
  booleanAt,
  fail,
  numberAt,
  objectAt,
  stringArrayAt,
  stringAt,
  type JsonObject,
} from './shared';

function oneOf<T extends string>(values: readonly T[], value: unknown, path: string): T {
  if (typeof value !== 'string' || !(values as readonly string[]).includes(value)) {
    return fail(path, `one of ${values.join(', ')}`);
  }
  return value as T;
}

function optionalString(record: JsonObject, key: string, path: string): string | undefined {
  return record[key] === undefined || record[key] === null
    ? undefined : stringAt(record[key], `${path}.${key}`);
}

export function parseCodedError(value: unknown, path: string): CodedError {
  const record = objectAt(value, path);
  return { code: stringAt(record.code, `${path}.code`), message: stringAt(record.message, `${path}.message`) };
}

function optionalCodedError(record: JsonObject, key: string, path: string): CodedError | undefined {
  return record[key] === undefined || record[key] === null
    ? undefined : parseCodedError(record[key], `${path}.${key}`);
}

function withVendor<T extends object>(record: JsonObject, path: string, base: T): T & { vendor_code?: string; vendor_text?: string } {
  const vendorCode = optionalString(record, 'vendor_code', path);
  const vendorText = optionalString(record, 'vendor_text', path);
  return {
    ...base,
    ...(vendorCode === undefined ? {} : { vendor_code: vendorCode }),
    ...(vendorText === undefined ? {} : { vendor_text: vendorText }),
  };
}

export function parseFault(value: unknown, path: string): Fault {
  const record = objectAt(value, path);
  return withVendor(record, path, {
    code: stringAt(record.code, `${path}.code`),
    message: stringAt(record.message, `${path}.message`),
    clearable: booleanAt(record.clearable, `${path}.clearable`),
    operator_action: oneOf(FAULT_OPERATOR_ACTIONS, record.operator_action, `${path}.operator_action`),
    ...(record.concerns === undefined || record.concerns === null
      ? {} : { concerns: stringArrayAt(record.concerns, `${path}.concerns`) }),
    since_ns: numberAt(record.since_ns, `${path}.since_ns`),
  });
}

function parseActionError(value: unknown, path: string): ActionError {
  const record = objectAt(value, path);
  return withVendor(record, path, parseCodedError(record, path));
}

export function parseActionResult(value: unknown, path: string): ActionResult {
  const record = objectAt(value, path);
  return {
    action: stringAt(record.action, `${path}.action`),
    outcome: oneOf(ACTION_OUTCOMES, record.outcome, `${path}.outcome`),
    ...(record.error === undefined || record.error === null
      ? {} : { error: parseActionError(record.error, `${path}.error`) }),
    started_at_ns: numberAt(record.started_at_ns, `${path}.started_at_ns`),
    finished_at_ns: numberAt(record.finished_at_ns, `${path}.finished_at_ns`),
  };
}

export function parseComponentState(value: unknown, path: string): ComponentState {
  const record = objectAt(value, path);
  const lifecycle = oneOf(COMPONENT_LIFECYCLES, record.lifecycle, `${path}.lifecycle`);
  const fault = record.fault === undefined || record.fault === null
    ? undefined : parseFault(record.fault, `${path}.fault`);
  const reason = optionalCodedError(record, 'reason', path);
  const action = record.action === undefined || record.action === null ? undefined : (() => {
    const row = objectAt(record.action, `${path}.action`);
    return {
      action: stringAt(row.action, `${path}.action.action`),
      started_at_ns: numberAt(row.started_at_ns, `${path}.action.started_at_ns`),
    };
  })();
  const ownerId = optionalString(record, 'owner_id', path);
  const role = optionalString(record, 'role', path);
  return {
    component_id: stringAt(record.component_id, `${path}.component_id`),
    ...(ownerId === undefined ? {} : { owner_id: ownerId }),
    ...(role === undefined ? {} : { role }),
    lifecycle,
    ...(fault === undefined ? {} : { fault }),
    ...(reason === undefined ? {} : { reason }),
    activity: oneOf(COMPONENT_ACTIVITIES, record.activity, `${path}.activity`),
    streams: arrayAt(record.streams, `${path}.streams`).map((item, index) => {
      const row = objectAt(item, `${path}.streams[${index}]`);
      return {
        stream_id: stringAt(row.stream_id, `${path}.streams[${index}].stream_id`),
        freshness: oneOf(STREAM_FRESHNESS, row.freshness, `${path}.streams[${index}].freshness`),
        ...(row.age_ns === undefined || row.age_ns === null
          ? {} : { age_ns: numberAt(row.age_ns, `${path}.streams[${index}].age_ns`) }),
      };
    }),
    capabilities: stringArrayAt(record.capabilities, `${path}.capabilities`),
    ...(action === undefined ? {} : { action }),
    ...(record.last_action === undefined || record.last_action === null
      ? {} : { last_action: parseActionResult(record.last_action, `${path}.last_action`) }),
    ...(record.details === undefined || record.details === null ? {} : {
      details: arrayAt(record.details, `${path}.details`).map((item, index) => {
        const row = objectAt(item, `${path}.details[${index}]`);
        return {
          name: stringAt(row.name, `${path}.details[${index}].name`),
          value: stringAt(row.value, `${path}.details[${index}].value`),
        };
      }),
    }),
    observed_at_ns: numberAt(record.observed_at_ns, `${path}.observed_at_ns`),
  };
}

export function parseComponentStates(value: unknown, path = 'components'): ComponentState[] {
  return arrayAt(value, path).map((item, index) => parseComponentState(item, `${path}[${index}]`));
}

export function parseCellLatch(value: unknown, path = 'cell_latch'): CellLatch {
  const record = objectAt(value, path);
  return {
    latched: booleanAt(record.latched, `${path}.latched`),
    causes: arrayAt(record.causes, `${path}.causes`).map((item, index) => {
      const row = objectAt(item, `${path}.causes[${index}]`);
      return {
        component_id: stringAt(row.component_id, `${path}.causes[${index}].component_id`),
        kind: oneOf(LATCH_CAUSE_KINDS, row.kind, `${path}.causes[${index}].kind`),
        ...(row.fault === undefined || row.fault === null
          ? {} : { fault: parseFault(row.fault, `${path}.causes[${index}].fault`) }),
      };
    }),
  };
}

function signalValue(value: unknown, path: string): boolean | number | null {
  if (value === null || typeof value === 'boolean') return value;
  return numberAt(value, path);
}

export function parseSignalSample(value: unknown, path: string): SignalSample {
  const record = objectAt(value, path);
  return {
    signal_id: stringAt(record.signal_id, `${path}.signal_id`),
    value: signalValue(record.value, `${path}.value`),
    source: oneOf(SIGNAL_SOURCES, record.source, `${path}.source`),
    age_ns: numberAt(record.age_ns, `${path}.age_ns`),
    fresh: booleanAt(record.fresh, `${path}.fresh`),
    ...(record.controller_owned === undefined
      ? {} : { controller_owned: booleanAt(record.controller_owned, `${path}.controller_owned`) }),
  };
}

export function parseSignalSamples(value: unknown, path = 'io_signals'): SignalSample[] {
  return arrayAt(value, path).map((item, index) => parseSignalSample(item, `${path}[${index}]`));
}

export function parseSetOutputResult(value: unknown, path = 'set_output'): SetOutputResult {
  const record = objectAt(value, path);
  const error = optionalCodedError(record, 'error', path);
  return {
    outcome: oneOf(ACTION_OUTCOMES, record.outcome, `${path}.outcome`),
    ...(error === undefined ? {} : { error }),
  };
}

export function parseRunReadiness(value: unknown, path = 'run_readiness'): RunReadiness {
  const record = objectAt(value, path);
  return {
    can_execute: booleanAt(record.can_execute, `${path}.can_execute`),
    can_resume: booleanAt(record.can_resume, `${path}.can_resume`),
    motion_clear: booleanAt(record.motion_clear, `${path}.motion_clear`),
    blocked_reasons: arrayAt(record.blocked_reasons, `${path}.blocked_reasons`).map((item, index) => {
      const at = `${path}.blocked_reasons[${index}]`;
      const row = objectAt(item, at);
      const reason: RunBlockedReason = parseCodedError(row, at);
      for (const key of ['component_id', 'rule_id', 'signal_id', 'scope'] as const) {
        const id = optionalString(row, key, at);
        if (id !== undefined) reason[key] = id;
      }
      if (row.component_ids !== undefined) {
        reason.component_ids = arrayAt(row.component_ids, `${at}.component_ids`)
          .map((id, idIndex) => stringAt(id, `${at}.component_ids[${idIndex}]`));
      }
      return reason;
    }),
  };
}

export function parseProgramRunEnd(value: unknown, path = 'run_end'): ProgramRunEnd {
  const record = objectAt(value, path);
  const details = objectAt(record.details, `${path}.details`);
  const error = optionalCodedError(details, 'error', `${path}.details`);
  const stoppedBy = details.stopped_by === undefined || details.stopped_by === null
    ? undefined : oneOf(['operator', 'rule'] as const, details.stopped_by, `${path}.details.stopped_by`);
  const parsed: ProgramRunEnd['details'] = {
    ...(error === undefined ? {} : { error }),
    ...(stoppedBy === undefined ? {} : { stopped_by: stoppedBy }),
  };
  for (const key of ['node_instance_id', 'rule_id', 'component_id'] as const) {
    const id = optionalString(details, key, `${path}.details`);
    if (id !== undefined) parsed[key] = id;
  }
  return { end_reason: oneOf(RUN_END_REASONS, record.end_reason, `${path}.end_reason`), details: parsed };
}

function nullableString(value: unknown, path: string): string | null {
  return value === null ? null : stringAt(value, path);
}

export function parseApplyPreview(value: unknown, path = 'apply_preview'): ApplyPreview {
  const record = objectAt(value, path);
  const program = objectAt(record.program, `${path}.program`);
  return {
    components: arrayAt(record.components, `${path}.components`).map((item, index) => {
      const at = `${path}.components[${index}]`;
      const row = objectAt(item, at);
      const rowError = optionalCodedError(row, 'error', at);
      return {
        ...(rowError === undefined ? {} : { error: rowError }),
        component_id: stringAt(row.component_id, `${at}.component_id`),
        disposition: oneOf(APPLY_DISPOSITIONS, row.disposition, `${at}.disposition`),
        reasons: stringArrayAt(row.reasons, `${at}.reasons`),
        restarts: booleanAt(row.restarts, `${at}.restarts`),
      };
    }),
    scope: objectAt(record.scope, `${path}.scope`),
    program: {
      executable: booleanAt(program.executable, `${path}.program.executable`),
      program_id: nullableString(program.program_id, `${path}.program.program_id`),
      unresolved_references: arrayAt(program.unresolved_references, `${path}.program.unresolved_references`)
        .map((item, index) => {
          const at = `${path}.program.unresolved_references[${index}]`;
          const row = objectAt(item, at);
          return {
            component_role_id: stringAt(row.component_role_id, `${at}.component_role_id`),
            target_role_id: nullableString(row.target_role_id, `${at}.target_role_id`),
            controllable_component_id: stringAt(row.controllable_component_id, `${at}.controllable_component_id`),
            command_target_id: nullableString(row.command_target_id, `${at}.command_target_id`),
            reason: stringAt(row.reason, `${at}.reason`),
            detail: stringAt(row.detail, `${at}.detail`),
          };
        }),
      affected_nodes: stringArrayAt(program.affected_nodes, `${path}.program.affected_nodes`),
      affected_execution_groups: stringArrayAt(
        program.affected_execution_groups, `${path}.program.affected_execution_groups`,
      ),
    },
    attachment_changed_components: stringArrayAt(
      record.attachment_changed_components, `${path}.attachment_changed_components`,
    ),
    detached_components: stringArrayAt(record.detached_components, `${path}.detached_components`),
  };
}

export function parseApplyResult(value: unknown, path = 'apply_result'): ApplyResult {
  const record = objectAt(value, path);
  const error = optionalCodedError(record, 'error', path);
  return {
    outcome: oneOf(['succeeded', 'failed'] as const, record.outcome, `${path}.outcome`),
    components: arrayAt(record.components, `${path}.components`).map((item, index) => {
      const at = `${path}.components[${index}]`;
      const row = objectAt(item, at);
      const rowError = optionalCodedError(row, 'error', at);
      return {
        component_id: stringAt(row.component_id, `${at}.component_id`),
        disposition: oneOf(APPLY_DISPOSITIONS, row.disposition, `${at}.disposition`),
        // A removed component has no lifecycle after the Apply.
        ...(row.lifecycle_after === undefined || row.lifecycle_after === null ? {} : {
          lifecycle_after: oneOf(COMPONENT_LIFECYCLES, row.lifecycle_after, `${at}.lifecycle_after`),
        }),
        // This component's failed step (the Apply continued with the others).
        ...(rowError === undefined ? {} : { error: rowError }),
      };
    }),
    ...(error === undefined ? {} : { error }),
  };
}

export function parseEffectiveValues(value: unknown, path = 'effective'): EffectiveValues {
  const record = objectAt(value, path);
  const fields = objectAt(record.fields, `${path}.fields`);
  return {
    fields: Object.fromEntries(Object.entries(fields).map(([key, item]) => {
      const row = objectAt(item, `${path}.fields.${key}`);
      if (!('value' in row) || !('default' in row)) fail(`${path}.fields.${key}`, 'an object with value and default');
      return [key, { value: row.value, default: row.default, set: booleanAt(row.set, `${path}.fields.${key}.set`) }];
    })),
  };
}
