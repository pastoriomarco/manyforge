import {
  allowedKeysAt,
  booleanAt,
  boundedArrayAt,
  exactKeysAt,
  fail,
  nonEmptyStringAt,
  nonnegativeInt64TextAt,
  nullablePositiveUint64TextAt,
  nullableStringAt,
  objectAt,
  stringAt,
} from './shared';

export type IoSignalQuality = 'missing' | 'fresh' | 'stale' | 'invalid' | 'unavailable';

/** The exact current-binding selector of one bound boolean signal. */
export interface IoSignalSelector {
  ownerEpoch: string;
  programBindingId: string;
  targetRoleId: string;
  resourceRoleId: string;
}

/**
 * An input's declared liveness (SX-2194, contracts 76/81), from the canonical
 * descriptor and present before the first value: `liveness_unproven` exactly
 * for a change-only source. Never inferred from quality or timestamps.
 */
export interface IoSignalLiveness {
  mode: 'in_process' | 'periodic' | 'change_only';
  warning: 'liveness_unproven' | null;
}

/** One `io_signal` target observation value (receiver clock). */
export interface IoSignalReading {
  /** Null for an output (no input liveness). */
  liveness: IoSignalLiveness | null;
  signalName: string;
  /** The last reported value; only `fresh` makes it current. */
  value: boolean | null;
  quality: IoSignalQuality;
  sourceId: string | null;
  sourceEpoch: string | null;
  sourceSequence: string | null;
  receiptAgeNs: number | null;
  continuityLossGeneration: string;
  errorCode: string | null;
}

const QUALITIES: readonly IoSignalQuality[] = ['missing', 'fresh', 'stale', 'invalid', 'unavailable'];

function receiptAgeAt(value: unknown, path: string): number | null {
  if (value === null) return null;
  if (typeof value === 'number' && Number.isSafeInteger(value) && value >= 0) return value;
  return Number(nonnegativeInt64TextAt(value, path));
}

/**
 * Parse one `query_target(kind=io_signal)` success. The observation must
 * echo the exact selector it answers.
 */
export function parseIoSignalResponse(
  value: unknown,
  command: 'query_target',
  selector: IoSignalSelector,
): IoSignalReading {
  const path = 'ioSignal';
  const envelope = objectAt(value, path);
  exactKeysAt(envelope, ['api_version', 'command', 'ok', 'target_observation'], path);
  return observationAt(envelope, command, selector, path);
}

/** The existing source cursor of one published Mock record (decimal-string counters). */
export interface IoSourceCursor {
  ownerEpoch: string;
  catalogGeneration: string;
  sourceId: string;
  recordSequence: string;
}

/**
 * One `set_mock_io_input` receipt (SX-2094): the model accepted
 * `modelValue`; `publishedCursor` names the record it was delivered as
 * through the real source, or is null while the source is not publishing
 * (e.g. Disabled: the model is kept and delivered on Enable). Only
 * `publicationError` means the model was accepted but ingress failed.
 * `reading` is the unchanged observation, which may stay unavailable.
 */
export interface MockIoInputReceipt {
  reading: IoSignalReading;
  modelValue: boolean;
  publishedCursor: IoSourceCursor | null;
  publicationError: { code: string; message: string } | null;
}

/** Parse one `set_mock_io_input` success for the request it answers. */
export function parseMockIoInputReceipt(
  value: unknown,
  selector: IoSignalSelector,
  requestedValue: boolean,
): MockIoInputReceipt {
  const path = 'mockIoInput';
  const envelope = objectAt(value, path);
  allowedKeysAt(envelope, ['api_version', 'command', 'ok', 'target_observation', 'model_value',
    'published_cursor'], ['publication_error'], path);
  const reading = observationAt(envelope, 'set_mock_io_input', selector, path);
  const modelValue = booleanAt(envelope.model_value, `${path}.model_value`);
  if (modelValue !== requestedValue) return fail(`${path}.model_value`, 'the requested value');
  let publishedCursor: IoSourceCursor | null = null;
  if (envelope.published_cursor !== null) {
    const at = `${path}.published_cursor`;
    const cursor = objectAt(envelope.published_cursor, at);
    exactKeysAt(cursor, ['owner_epoch', 'catalog_generation', 'source_id', 'record_sequence'], at);
    publishedCursor = {
      ownerEpoch: stringAt(cursor.owner_epoch, `${at}.owner_epoch`),
      catalogGeneration: nullablePositiveUint64TextAt(stringAt(cursor.catalog_generation, `${at}.catalog_generation`),
        `${at}.catalog_generation`)!,
      sourceId: stringAt(cursor.source_id, `${at}.source_id`),
      recordSequence: nullablePositiveUint64TextAt(stringAt(cursor.record_sequence, `${at}.record_sequence`),
        `${at}.record_sequence`)!,
    };
    if (publishedCursor.ownerEpoch !== selector.ownerEpoch) return fail(`${at}.owner_epoch`, 'the request owner epoch');
    if (reading.sourceId !== null && publishedCursor.sourceId !== reading.sourceId) {
      return fail(`${at}.source_id`, 'the observed source');
    }
  }
  let publicationError: MockIoInputReceipt['publicationError'] = null;
  if ('publication_error' in envelope) {
    const at = `${path}.publication_error`;
    const error = objectAt(envelope.publication_error, at);
    const code = stringAt(error.code, `${at}.code`);
    const message = stringAt(error.message, `${at}.message`);
    if (code === '') return fail(`${at}.code`, 'a non-empty string');
    if (publishedCursor !== null) return fail(at, 'absent when the value was published');
    publicationError = { code, message };
  }
  return { reading, modelValue, publishedCursor, publicationError };
}

function observationAt(
  envelope: Record<string, unknown>,
  command: 'query_target' | 'set_mock_io_input',
  selector: IoSignalSelector,
  path: string,
): IoSignalReading {
  if (envelope.api_version !== 'manyforge.execution_command_response.v1'
    || envelope.command !== command || envelope.ok !== true) {
    return fail(path, `a successful ${command} response`);
  }
  const observation = objectAt(envelope.target_observation, `${path}.target_observation`);
  exactKeysAt(observation, ['schema_version', 'owner_epoch', 'program_binding_id', 'target_role_id',
    'kind', 'resource_role_id', 'evaluated_at_ns', 'value'], `${path}.target_observation`);
  // The owner evaluation time (receiver clock): required, not displayed.
  receiptAgeAt(observation.evaluated_at_ns, `${path}.target_observation.evaluated_at_ns`)
    ?? fail(`${path}.target_observation.evaluated_at_ns`, 'a nonnegative int64');
  if (observation.schema_version !== 'manyforge.target_observation.v1'
    || observation.kind !== 'io_signal'
    || observation.owner_epoch !== selector.ownerEpoch
    || observation.program_binding_id !== selector.programBindingId
    || observation.target_role_id !== selector.targetRoleId
    || observation.resource_role_id !== selector.resourceRoleId) {
    return fail(`${path}.target_observation`, 'the I/O signal observation of this exact selector');
  }
  const at = `${path}.target_observation.value`;
  const reading = objectAt(observation.value, at);
  exactKeysAt(reading, ['liveness', 'signal_name', 'value_type', 'value', 'quality', 'source_id', 'source_epoch',
    'source_sequence', 'receipt_age_ns', 'continuity_loss_generation', 'error'], at);
  if (reading.value_type !== 'bool') return fail(`${at}.value_type`, 'bool');
  const quality = reading.quality as IoSignalQuality;
  if (!QUALITIES.includes(quality)) return fail(`${at}.quality`, QUALITIES.join(' | '));
  const signalValue = reading.value === null ? null : booleanAt(reading.value, `${at}.value`);
  // The opaque source incarnation (spec 260 §2.3), not a counter.
  const sourceEpoch = reading.source_epoch === null
    ? null
    : sourceEpochAt(reading.source_epoch, `${at}.source_epoch`);
  const sourceSequence = nullablePositiveUint64TextAt(reading.source_sequence, `${at}.source_sequence`);
  const receiptAgeNs = receiptAgeAt(reading.receipt_age_ns, `${at}.receipt_age_ns`);
  if (quality === 'fresh'
    && (signalValue === null || sourceEpoch === null || sourceSequence === null || receiptAgeNs === null)) {
    return fail(at, 'a fresh observation with its value and sample');
  }
  const error = reading.error === null ? null : objectAt(reading.error, `${at}.error`);
  return {
    liveness: livenessAt(reading.liveness, `${at}.liveness`),
    signalName: stringAt(reading.signal_name, `${at}.signal_name`),
    value: signalValue,
    quality,
    sourceId: nullableStringAt(reading.source_id, `${at}.source_id`),
    sourceEpoch,
    sourceSequence,
    receiptAgeNs,
    continuityLossGeneration: reading.continuity_loss_generation === '0'
      ? '0'
      : nullablePositiveUint64TextAt(reading.continuity_loss_generation, `${at}.continuity_loss_generation`)
        ?? fail(`${at}.continuity_loss_generation`, 'a canonical uint64 decimal string'),
    errorCode: error === null || typeof error.code !== 'string' ? null : error.code,
  };
}

/** The same bounded opaque identity the headless model accepts. */
function sourceEpochAt(value: unknown, field: string): string {
  const result = stringAt(value, field);
  if (result.length === 0 || result !== result.normalize('NFC')
    || new TextEncoder().encode(result).length > 256) {
    return fail(field, 'a non-empty NFC-normalized identifier within 256 UTF-8 bytes');
  }
  return result;
}

function livenessAt(value: unknown, path: string): IoSignalLiveness | null {
  if (value === null) return null;
  const liveness = objectAt(value, path);
  exactKeysAt(liveness, ['mode', 'warning'], path);
  const { mode, warning } = liveness;
  if (mode !== 'in_process' && mode !== 'periodic' && mode !== 'change_only') {
    return fail(`${path}.mode`, 'in_process | periodic | change_only');
  }
  if (warning !== (mode === 'change_only' ? 'liveness_unproven' : null)) {
    return fail(`${path}.warning`, mode === 'change_only' ? 'liveness_unproven' : 'null');
  }
  return { mode, warning: mode === 'change_only' ? 'liveness_unproven' : null };
}

export function ioSignalReadBody(selector: IoSignalSelector) {
  return {
    schema_version: 'manyforge.target_observation_query.v1',
    owner_epoch: selector.ownerEpoch,
    program_binding_id: selector.programBindingId,
    target_role_id: selector.targetRoleId,
    kind: 'io_signal',
    resource_role_id: selector.resourceRoleId,
  } as const;
}

export function mockIoInputBody(selector: IoSignalSelector, expectedSourceEpoch: string, value: boolean) {
  return {
    schema_version: 'manyforge.mock_io_input_request.v1',
    owner_epoch: selector.ownerEpoch,
    program_binding_id: selector.programBindingId,
    target_role_id: selector.targetRoleId,
    resource_role_id: selector.resourceRoleId,
    expected_source_epoch: expectedSourceEpoch,
    value,
  } as const;
}

/** One target's bound signals: the `targets.io_signal_bindings` row shape. */
export interface IoSignalBinding {
  programBindingId: string;
  targetRoleId: string;
  controllableComponentId: string;
  commandTargetId: string;
  signalRoleBindings: Array<{
    resourceRoleId: string;
    signalName: string;
    valueType: 'bool';
    direction: 'input' | 'output';
    transitionCapability: 'sampled_only' | 'captured';
    signalDescriptorDigest: string;
  }>;
}

/** One generated read-only signal from `targets.system_signals`. */
export interface SystemSignalProjection {
  controllableComponentId: string;
  commandTargetId: string;
  signalName: string;
  sourceKind: 'system_carried_object';
  direction: 'input';
  valueType: 'bool';
  level: boolean;
}

/** Parse optional `targets.system_signals`, ordered by component and target. */
export function systemSignalsAt(value: unknown, path: string): SystemSignalProjection[] {
  const rows = boundedArrayAt(value, path, 4096);
  const parsed = rows.map((entry, index): SystemSignalProjection => {
    const at = `${path}[${index}]`;
    const row = objectAt(entry, at);
    exactKeysAt(row, [
      'controllable_component_id', 'command_target_id', 'signal_name', 'source_kind',
      'direction', 'value_type', 'level',
    ], at);
    if (row.source_kind !== 'system_carried_object') {
      return fail(`${at}.source_kind`, 'system_carried_object');
    }
    if (row.direction !== 'input') return fail(`${at}.direction`, 'input');
    if (row.value_type !== 'bool') return fail(`${at}.value_type`, 'bool');
    return {
      controllableComponentId: nonEmptyStringAt(row.controllable_component_id, `${at}.controllable_component_id`),
      commandTargetId: nonEmptyStringAt(row.command_target_id, `${at}.command_target_id`),
      signalName: nonEmptyStringAt(row.signal_name, `${at}.signal_name`),
      sourceKind: 'system_carried_object',
      direction: 'input',
      valueType: 'bool',
      level: booleanAt(row.level, `${at}.level`),
    };
  });
  const seen = new Set<string>();
  parsed.forEach((row, index) => {
    const identity = JSON.stringify([row.controllableComponentId, row.commandTargetId, row.signalName]);
    if (seen.has(identity)) return fail(path, 'unique system signal rows');
    seen.add(identity);
    if (index === 0) return;
    const previous = parsed[index - 1];
    if (previous.controllableComponentId > row.controllableComponentId
      || (previous.controllableComponentId === row.controllableComponentId
        && previous.commandTargetId > row.commandTargetId)) {
      fail(path, 'rows sorted by controllable_component_id and command_target_id');
    }
  });
  return parsed;
}

const MAXIMUM_IO_SIGNAL_BINDING_ROWS = 4096;

/**
 * Parse optional `targets.io_signal_bindings` (execution_targets.v3): a
 * non-empty array in canonical component/command target/target role/binding
 * order, each with its sorted, non-empty signal rows.
 */
export function ioSignalBindingsAt(value: unknown, path: string): IoSignalBinding[] {
  if (!Array.isArray(value) || value.length === 0 || value.length > MAXIMUM_IO_SIGNAL_BINDING_ROWS) {
    return fail(path, `a non-empty array of at most ${MAXIMUM_IO_SIGNAL_BINDING_ROWS} rows`);
  }
  const rows = value.map((entry, index): IoSignalBinding => {
    const at = `${path}[${index}]`;
    const row = objectAt(entry, at);
    allowedKeysAt(row, ['program_binding_id', 'target_role_id', 'controllable_component_id',
      'command_target_id', 'signal_role_bindings'], ['signal_sources'], at);
    // Host-owned provider projection (SX/DX-1494): validated for shape, not consumed.
    const sources = row.signal_sources === undefined ? [] : row.signal_sources;
    if (!Array.isArray(sources)) return fail(`${at}.signal_sources`, 'an array');
    sources.forEach((item, sourceIndex) => {
      const sp = `${at}.signal_sources[${sourceIndex}]`;
      const source = objectAt(item, sp);
      exactKeysAt(source, ['resource_role_id', 'source_kind'], sp);
      stringAt(source.resource_role_id, `${sp}.resource_role_id`);
      stringAt(source.source_kind, `${sp}.source_kind`);
    });
    const signals = row.signal_role_bindings;
    if (!Array.isArray(signals) || signals.length === 0) return fail(`${at}.signal_role_bindings`, 'a non-empty array');
    const signalRoleBindings = signals.map((item, signalIndex) => {
      const sp = `${at}.signal_role_bindings[${signalIndex}]`;
      const signal = objectAt(item, sp);
      exactKeysAt(signal, ['resource_role_id', 'signal_name', 'value_type', 'direction',
        'transition_capability', 'signal_descriptor_digest'], sp);
      if (signal.value_type !== 'bool') return fail(`${sp}.value_type`, 'bool');
      if (signal.direction !== 'input' && signal.direction !== 'output') return fail(`${sp}.direction`, 'input | output');
      if (signal.transition_capability !== 'sampled_only' && signal.transition_capability !== 'captured') {
        return fail(`${sp}.transition_capability`, 'sampled_only | captured');
      }
      const digest = stringAt(signal.signal_descriptor_digest, `${sp}.signal_descriptor_digest`);
      if (!/^sha256:[0-9a-f]{64}$/.test(digest)) return fail(`${sp}.signal_descriptor_digest`, 'a sha256 digest');
      return {
        resourceRoleId: stringAt(signal.resource_role_id, `${sp}.resource_role_id`),
        signalName: stringAt(signal.signal_name, `${sp}.signal_name`),
        valueType: 'bool' as const,
        direction: signal.direction as 'input' | 'output',
        transitionCapability: signal.transition_capability as 'sampled_only' | 'captured',
        signalDescriptorDigest: digest,
      };
    });
    signalRoleBindings.forEach((signal, signalIndex) => {
      if (signalIndex > 0 && !(signalRoleBindings[signalIndex - 1].resourceRoleId < signal.resourceRoleId)) {
        fail(`${at}.signal_role_bindings`, 'unique rows sorted by resource_role_id');
      }
    });
    const programBindingId = stringAt(row.program_binding_id, `${at}.program_binding_id`);
    if (!/^sha256:[0-9a-f]{64}$/.test(programBindingId)) return fail(`${at}.program_binding_id`, 'a sha256 digest');
    return {
      programBindingId,
      targetRoleId: stringAt(row.target_role_id, `${at}.target_role_id`),
      controllableComponentId: stringAt(row.controllable_component_id, `${at}.controllable_component_id`),
      commandTargetId: stringAt(row.command_target_id, `${at}.command_target_id`),
      signalRoleBindings,
    };
  });
  const order = (row: IoSignalBinding) => [row.controllableComponentId, row.commandTargetId,
    row.targetRoleId, row.programBindingId];
  rows.forEach((row, index) => {
    if (index === 0) return;
    const previous = order(rows[index - 1]);
    const current = order(row);
    const compared = previous.findIndex((item, part) => item !== current[part]);
    if (compared === -1 || !(previous[compared] < current[compared])) {
      fail(path, 'unique rows in component/command target/target role/binding order');
    }
  });
  return rows;
}

/**
 * Interim source of the same rows: one Program binding document's
 * `signal_role_bindings`, adapted to the projection shape.
 */
export function ioSignalBindingsFromProgramBinding(binding: {
  program_binding_id: string;
  target_role_bindings: Array<{
    target_role_id: string;
    controllable_component_id: string;
    command_target_id: string;
    signal_role_bindings?: Array<{
      resource_role_id: string;
      signal_name: string;
      value_type: 'bool';
      direction: 'input' | 'output';
      transition_capability: 'sampled_only' | 'captured';
      signal_descriptor_digest: string;
    }>;
  }>;
}): IoSignalBinding[] {
  return binding.target_role_bindings.flatMap((target) => (
    (target.signal_role_bindings ?? []).length === 0 ? [] : [{
      programBindingId: binding.program_binding_id,
      targetRoleId: target.target_role_id,
      controllableComponentId: target.controllable_component_id,
      commandTargetId: target.command_target_id,
      signalRoleBindings: target.signal_role_bindings!.map((row) => ({
        resourceRoleId: row.resource_role_id,
        signalName: row.signal_name,
        valueType: row.value_type,
        direction: row.direction,
        transitionCapability: row.transition_capability,
        signalDescriptorDigest: row.signal_descriptor_digest,
      })),
    }]
  ));
}
