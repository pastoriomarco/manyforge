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
  componentId: string;
  signalName: string;
}

/**
 * An input's declared liveness, from the canonical
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
 * One `set_mock_io_input` receipt: the model accepted
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
  if (envelope.api_version !== 'manyforge.execution_command_response.v4'
    || envelope.command !== command || envelope.ok !== true) {
    return fail(path, `a successful ${command} response`);
  }
  const observation = objectAt(envelope.target_observation, `${path}.target_observation`);
  exactKeysAt(observation, ['schema_version', 'owner_epoch', 'program_binding_id', 'signal',
    'kind', 'evaluated_at_ns', 'value'], `${path}.target_observation`);
  const signal = objectAt(observation.signal, `${path}.target_observation.signal`);
  exactKeysAt(signal, ['component_id', 'signal_name'], `${path}.target_observation.signal`);
  // The owner evaluation time (receiver clock): required, not displayed.
  if (receiptAgeAt(observation.evaluated_at_ns, `${path}.target_observation.evaluated_at_ns`) === null) {
    fail(`${path}.target_observation.evaluated_at_ns`, 'a nonnegative int64');
  }
  if (observation.schema_version !== 'manyforge.target_observation.v2'
    || observation.kind !== 'io_signal'
    || observation.owner_epoch !== selector.ownerEpoch
    || observation.program_binding_id !== selector.programBindingId
    || signal.component_id !== selector.componentId
    || signal.signal_name !== selector.signalName) {
    return fail(`${path}.target_observation`, 'the I/O signal observation of this exact selector');
  }
  const at = `${path}.target_observation.value`;
  const reading = objectAt(observation.value, at);
  exactKeysAt(reading, ['liveness', 'signal_name', 'value_type', 'value', 'quality', 'source_id', 'source_epoch',
    'source_sequence', 'receipt_age_ns', 'continuity_loss_generation', 'error'], at);
  if (reading.signal_name !== selector.signalName) return fail(`${at}.signal_name`, 'the selected signal');
  if (reading.value_type !== 'bool') return fail(`${at}.value_type`, 'bool');
  const quality = reading.quality as IoSignalQuality;
  if (!QUALITIES.includes(quality)) return fail(`${at}.quality`, QUALITIES.join(' | '));
  const signalValue = reading.value === null ? null : booleanAt(reading.value, `${at}.value`);
  // The opaque source incarnation, not a counter.
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
    schema_version: 'manyforge.target_observation_query.v2',
    owner_epoch: selector.ownerEpoch,
    program_binding_id: selector.programBindingId,
    kind: 'io_signal',
    signal: { component_id: selector.componentId, signal_name: selector.signalName },
  } as const;
}

export function mockIoInputBody(selector: IoSignalSelector, expectedSourceEpoch: string, value: boolean) {
  return {
    schema_version: 'manyforge.mock_io_input_request.v2',
    owner_epoch: selector.ownerEpoch,
    program_binding_id: selector.programBindingId,
    signal: { component_id: selector.componentId, signal_name: selector.signalName },
    expected_source_epoch: expectedSourceEpoch,
    value,
  } as const;
}

/** One target's bound signals: the `targets.io_signal_bindings` row shape. */
export interface IoSignalBinding {
  programBindingId: string;
  componentId: string;
  commandTargetId: string;
  signals: Array<{
    componentId: string;
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
  if (!Array.isArray(value) || value.length === 0 || value.length > MAXIMUM_IO_SIGNAL_BINDING_ROWS)
    return fail(path, `a non-empty array of at most ${MAXIMUM_IO_SIGNAL_BINDING_ROWS} rows`);
  const rows = value.map((entry, index): IoSignalBinding => {
    const at = `${path}[${index}]`;
    const row = objectAt(entry, at);
    exactKeysAt(row, ['program_binding_id', 'target', 'signals', 'signal_sources'], at);
    const target = objectAt(row.target, `${at}.target`);
    exactKeysAt(target, ['component_id', 'command_target_id'], `${at}.target`);
    const componentId = nonEmptyStringAt(target.component_id, `${at}.target.component_id`);
    const commandTargetId = nonEmptyStringAt(target.command_target_id, `${at}.target.command_target_id`);
    const programBindingId = stringAt(row.program_binding_id, `${at}.program_binding_id`);
    if (!/^sha256:[0-9a-f]{64}$/.test(programBindingId)) return fail(`${at}.program_binding_id`, 'a sha256 digest');
    if (!Array.isArray(row.signals) || row.signals.length === 0) return fail(`${at}.signals`, 'a non-empty array');
    const signals = row.signals.map((item, signalIndex) => {
      const sp = `${at}.signals[${signalIndex}]`;
      const signal = objectAt(item, sp);
      exactKeysAt(signal, ['reference', 'target', 'runtime_unit_id', 'value_type', 'direction',
        'transition_capability', 'signal_descriptor_digest'], sp);
      const ref = objectAt(signal.reference, `${sp}.reference`);
      exactKeysAt(ref, ['component_id', 'signal_name'], `${sp}.reference`);
      const selected = objectAt(signal.target, `${sp}.target`);
      exactKeysAt(selected, ['component_id', 'command_target_id'], `${sp}.target`);
      if (selected.component_id !== componentId || selected.command_target_id !== commandTargetId
          || ref.component_id !== componentId) return fail(sp, 'the exact selected target and component');
      nonEmptyStringAt(signal.runtime_unit_id, `${sp}.runtime_unit_id`);
      if (signal.value_type !== 'bool') return fail(`${sp}.value_type`, 'bool');
      if (signal.direction !== 'input' && signal.direction !== 'output') return fail(`${sp}.direction`, 'input | output');
      if (signal.transition_capability !== 'sampled_only' && signal.transition_capability !== 'captured')
        return fail(`${sp}.transition_capability`, 'sampled_only | captured');
      const digest = stringAt(signal.signal_descriptor_digest, `${sp}.signal_descriptor_digest`);
      if (!/^sha256:[0-9a-f]{64}$/.test(digest)) return fail(`${sp}.signal_descriptor_digest`, 'a sha256 digest');
      return { componentId: nonEmptyStringAt(ref.component_id, `${sp}.reference.component_id`),
        signalName: nonEmptyStringAt(ref.signal_name, `${sp}.reference.signal_name`), valueType: 'bool' as const,
        direction: signal.direction as 'input' | 'output',
        transitionCapability: signal.transition_capability as 'sampled_only' | 'captured', signalDescriptorDigest: digest };
    });
    for (let i = 1; i < signals.length; i++) {
      if (!(signals[i - 1].signalName < signals[i].signalName)) return fail(`${at}.signals`, 'unique sorted signals');
    }
    if (!Array.isArray(row.signal_sources)) return fail(`${at}.signal_sources`, 'an array');
    row.signal_sources.forEach((item, sourceIndex) => {
      const sp = `${at}.signal_sources[${sourceIndex}]`;
      const source = objectAt(item, sp);
      exactKeysAt(source, ['signal', 'source_kind'], sp);
      const ref = objectAt(source.signal, `${sp}.signal`);
      exactKeysAt(ref, ['component_id', 'signal_name'], `${sp}.signal`);
      nonEmptyStringAt(ref.component_id, `${sp}.signal.component_id`);
      nonEmptyStringAt(ref.signal_name, `${sp}.signal.signal_name`);
      nonEmptyStringAt(source.source_kind, `${sp}.source_kind`);
    });
    return { programBindingId, componentId, commandTargetId, signals };
  });
  for (let i = 1; i < rows.length; i++) {
    const a = JSON.stringify([rows[i - 1].componentId, rows[i - 1].commandTargetId, rows[i - 1].programBindingId]);
    const b = JSON.stringify([rows[i].componentId, rows[i].commandTargetId, rows[i].programBindingId]);
    if (!(a < b)) return fail(path, 'unique rows in component/command target/binding order');
  }
  return rows;
}

/** Project exact signal references through one already validated Applied Binding. */
export function ioSignalBindingsFromProgramBinding(
  binding: import('../../types/programBindingTransport.generated').ProgramBindingDocument,
): IoSignalBinding[] {
  return binding.target_bindings.flatMap((target) => {
    const signals = target.signal_references.map((reference) => {
      const rows = binding.signal_bindings.filter((row) => row.reference.component_id === reference.component_id
        && row.reference.signal_name === reference.signal_name
        && row.target.component_id === target.target.component_id
        && row.target.command_target_id === target.target.command_target_id);
      if (rows.length !== 1) throw new Error('Applied Binding signal reference has no exact target resolution');
      const row = rows[0];
      return { componentId: row.reference.component_id, signalName: row.reference.signal_name,
        valueType: row.value_type, direction: row.direction, transitionCapability: row.transition_capability,
        signalDescriptorDigest: row.signal_descriptor_digest };
    });
    return signals.length === 0 ? [] : [{ programBindingId: binding.program_binding_id,
      componentId: target.target.component_id, commandTargetId: target.target.command_target_id, signals }];
  });
}
