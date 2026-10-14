import type {
  HistorianEvent,
  HistorianEventSummary,
  HistorianDetail,
  HistorianPage,
  HistorianDiscovery,
  HistorianRetention,
  HistorianCapture,
  HistorianCapturePage,
} from '../../types/headless';
import {
  arrayAt,
  booleanAt,
  exactKeysAt,
  fail,
  integerAt,
  nonnegativeInt64TextAt,
  nullableStringAt,
  objectAt,
  positiveIntegerAt,
  stringAt,
} from './shared';

function parseHistorianEvent(value: unknown, path: string): HistorianEvent {
  const record = objectAt(value, path);
  const schemaVersion = stringAt(record.schema_version, `${path}.schema_version`);
  if (schemaVersion !== 'manyforge.historian_event.v2') {
    return fail(`${path}.schema_version`, 'manyforge.historian_event.v2');
  }
  return {
    schemaVersion,
    eventId: stringAt(record.event_id, `${path}.event_id`),
    component: stringAt(record.component, `${path}.component`),
    eventType: stringAt(record.event_type, `${path}.event_type`),
    severity: stringAt(record.severity, `${path}.severity`) as 'error',
    occurredAtNs: nonnegativeInt64TextAt(
      record.occurred_at_ns,
      `${path}.occurred_at_ns`,
    ),
    occurredAtWall: stringAt(
      record.occurred_at_wall,
      `${path}.occurred_at_wall`,
    ),
    payload: objectAt(record.payload, `${path}.payload`),
  };
}

function parseHistorianEventSummary(
  value: unknown,
  path: string,
): HistorianEventSummary {
  const record = objectAt(value, path);
  exactKeysAt(record, [
    'component',
    'condition_id',
    'event_id',
    'event_type',
    'message',
    'occurred_at_ns',
    'occurred_at_wall',
    'operation_id',
    'program_run_id',
    'schema_version',
    'severity',
    'condition_severity',
    'occurred_at_utc_ns',
    'clock_owner',
    'wall_clock_regressed',
  ], path);
  const schemaVersion = stringAt(record.schema_version, `${path}.schema_version`);
  if (schemaVersion !== 'manyforge.historian_event_summary.v2') {
    return fail(`${path}.schema_version`, 'manyforge.historian_event_summary.v2');
  }
  return {
    schemaVersion,
    eventId: stringAt(record.event_id, `${path}.event_id`),
    component: stringAt(record.component, `${path}.component`),
    eventType: stringAt(record.event_type, `${path}.event_type`),
    severity: stringAt(record.severity, `${path}.severity`),
    occurredAtNs: nonnegativeInt64TextAt(
      record.occurred_at_ns,
      `${path}.occurred_at_ns`,
    ),
    occurredAtWall: stringAt(record.occurred_at_wall, `${path}.occurred_at_wall`),
    message: stringAt(record.message, `${path}.message`),
    programRunId: nullableStringAt(record.program_run_id, `${path}.program_run_id`),
    operationId: nullableStringAt(record.operation_id, `${path}.operation_id`),
    conditionId: nullableStringAt(record.condition_id, `${path}.condition_id`),
    conditionSeverity: stringAt(record.condition_severity, `${path}.condition_severity`),
    occurredAtUtcNs: record.occurred_at_utc_ns === null ? null : nonnegativeInt64TextAt(record.occurred_at_utc_ns, `${path}.occurred_at_utc_ns`),
    clockOwner: nullableStringAt(record.clock_owner, `${path}.clock_owner`),
    wallClockRegressed: booleanAt(record.wall_clock_regressed, `${path}.wall_clock_regressed`),
  };
}

function parseHistorianRetention(value: unknown): HistorianRetention | null {
  if (value === null) return null;
  const path = 'history.retention';
  const record = objectAt(value, path);
  exactKeysAt(record, ['schema_version', 'partitions', 'captures', 'partition_row_limit', 'partition_record_byte_limit', 'database_page_byte_limit', 'duration_guaranteed', 'prior_pruning_known'], path);
  if (record.schema_version !== 'manyforge.historian_retention.v2' || record.duration_guaranteed !== false) {
    return fail(path, 'the bounded local retention v2 profile');
  }
  const partitions = arrayAt(record.partitions, `${path}.partitions`);
  if (partitions.length !== 2) return fail(path, 'routine and important retention partitions');
  const captures = objectAt(record.captures, `${path}.captures`);
  exactKeysAt(captures, ['live_count', 'open_count', 'protected_count', 'protected_bytes', 'live_capture_limit', 'protected_byte_limit', 'retired_count', 'released_rows', 'released_bytes', 'last_retired_capture_id'], `${path}.captures`);
  const captureScalar = (key: string) => nonnegativeInt64TextAt(captures[key], `${path}.captures.${key}`);
  return {
    schemaVersion: record.schema_version,
    captures: {
      liveCount: captureScalar('live_count'),
      openCount: captureScalar('open_count'),
      protectedCount: captureScalar('protected_count'),
      protectedBytes: captureScalar('protected_bytes'),
      liveCaptureLimit: captureScalar('live_capture_limit'),
      protectedByteLimit: captureScalar('protected_byte_limit'),
      retiredCount: captureScalar('retired_count'),
      releasedRows: captureScalar('released_rows'),
      releasedBytes: captureScalar('released_bytes'),
      lastRetiredCaptureId: captures.last_retired_capture_id === null
        ? null : captureScalar('last_retired_capture_id'),
    },
    partitionRowLimit: nonnegativeInt64TextAt(record.partition_row_limit, `${path}.partition_row_limit`),
    partitionRecordByteLimit: nonnegativeInt64TextAt(record.partition_record_byte_limit, `${path}.partition_record_byte_limit`),
    databasePageByteLimit: nonnegativeInt64TextAt(record.database_page_byte_limit, `${path}.database_page_byte_limit`),
    durationGuaranteed: false,
    priorPruningKnown: booleanAt(record.prior_pruning_known, `${path}.prior_pruning_known`),
    partitions: partitions.map((value, index) => {
      const partPath = `${path}.partitions[${index}]`;
      const part = objectAt(value, partPath);
      exactKeysAt(part, ['partition', 'retained_count', 'retained_bytes', 'pruned_count', 'pruned_bytes', 'last_pruned_sequence', 'unknown_time_count', 'regressed_time_count', 'loss_counter_saturated', 'first_recorded_utc_ns', 'last_recorded_utc_ns'], partPath);
      const partition = index === 0 ? 'routine' : 'important';
      if (part.partition !== partition) return fail(partPath, partition);
      const scalar = (key: string) => nonnegativeInt64TextAt(part[key], `${partPath}.${key}`);
      const nullable = (key: string) => part[key] === null ? null : scalar(key);
      return {
        partition, retainedCount: scalar('retained_count'), retainedBytes: scalar('retained_bytes'),
        prunedCount: scalar('pruned_count'), prunedBytes: scalar('pruned_bytes'),
        lastPrunedSequence: nullable('last_pruned_sequence'), unknownTimeCount: scalar('unknown_time_count'),
        regressedTimeCount: scalar('regressed_time_count'),
        lossCounterSaturated: booleanAt(part.loss_counter_saturated, `${partPath}.loss_counter_saturated`),
        firstRecordedUtcNs: nullable('first_recorded_utc_ns'), lastRecordedUtcNs: nullable('last_recorded_utc_ns'),
      };
    }),
  };
}

const CAPTURE_STATES = ['open', 'closed', 'truncated', 'closed_by_restart'] as const;
const CAPTURE_CLOSE_REASONS = ['tail_rows', 'tail_duration', 'capture_bytes', 'restart'] as const;
const CAPTURE_PRECEDING_LIMITS = ['rows', 'bytes', 'history_start', 'pruned'] as const;
const CAPTURE_UNCERTAINTY = [
  'context_pruned_before_capture',
  'writer_delivery_loss_before_capture',
  'writer_delivery_loss_during_capture',
  'context_spans_writer_restart',
  'tail_interrupted_by_restart',
] as const;

function oneOf<T extends string>(value: unknown, allowed: readonly T[], path: string): T {
  const text = stringAt(value, path);
  return (allowed as readonly string[]).includes(text)
    ? text as T
    : fail(path, allowed.join(' | '));
}

function parseHistorianCapture(value: unknown, path: string): HistorianCapture {
  const record = objectAt(value, path);
  exactKeysAt(record, [
    'capture_id', 'state', 'close_reason', 'closure_recorded', 'first_sequence', 'trigger_sequence',
    'last_sequence', 'trigger_event_id', 'trigger', 'opened_at_utc_ns', 'preceding_rows',
    'preceding_limit', 'tail_rows', 'capture_bytes', 'trigger_count', 'recorded_triggers',
    'unrecorded_trigger_count', 'retained_rows', 'missing_sequences', 'writer_delivery_loss_before',
    'writer_delivery_loss_during', 'coverage', 'uncertainty', 'contents',
  ], path);
  const count = (key: string) => {
    const number = integerAt(record[key], `${path}.${key}`);
    return number < 0 ? fail(`${path}.${key}`, 'a nonnegative count') : number;
  };
  const trigger = record.trigger === null ? null : objectAt(record.trigger, `${path}.trigger`);
  const triggerText = (key: string) => trigger === null || trigger[key] === undefined || trigger[key] === null
    ? null : stringAt(trigger[key], `${path}.trigger.${key}`);
  const firstSequence = positiveIntegerAt(record.first_sequence, `${path}.first_sequence`);
  const triggerSequence = positiveIntegerAt(record.trigger_sequence, `${path}.trigger_sequence`);
  const lastSequence = positiveIntegerAt(record.last_sequence, `${path}.last_sequence`);
  if (!(firstSequence <= triggerSequence && triggerSequence <= lastSequence)) {
    return fail(path, 'first <= trigger <= last sequence');
  }
  const uncertainty = arrayAt(record.uncertainty, `${path}.uncertainty`)
    .map((item, index) => oneOf(item, CAPTURE_UNCERTAINTY, `${path}.uncertainty[${index}]`));
  const coverage = oneOf(record.coverage, ['complete', 'uncertain'] as const, `${path}.coverage`);
  if ((coverage === 'complete') !== (uncertainty.length === 0)) {
    return fail(`${path}.coverage`, 'complete only without uncertainty');
  }
  if (record.contents !== 'retained_event_context') return fail(`${path}.contents`, 'retained_event_context');
  return {
    captureId: nonnegativeInt64TextAt(record.capture_id, `${path}.capture_id`),
    state: oneOf(record.state, CAPTURE_STATES, `${path}.state`),
    closeReason: record.close_reason === null
      ? null : oneOf(record.close_reason, CAPTURE_CLOSE_REASONS, `${path}.close_reason`),
    closureRecorded: booleanAt(record.closure_recorded, `${path}.closure_recorded`),
    firstSequence,
    triggerSequence,
    lastSequence,
    triggerEventId: stringAt(record.trigger_event_id, `${path}.trigger_event_id`),
    trigger: trigger === null ? null : {
      eventType: triggerText('event_type'),
      component: triggerText('component'),
      conditionSeverity: triggerText('condition_severity'),
      message: triggerText('message'),
    },
    openedAtUtcNs: record.opened_at_utc_ns === null
      ? null : nonnegativeInt64TextAt(record.opened_at_utc_ns, `${path}.opened_at_utc_ns`),
    precedingRows: count('preceding_rows'),
    precedingLimit: oneOf(record.preceding_limit, CAPTURE_PRECEDING_LIMITS, `${path}.preceding_limit`),
    tailRows: count('tail_rows'),
    captureBytes: nonnegativeInt64TextAt(record.capture_bytes, `${path}.capture_bytes`),
    triggerCount: count('trigger_count'),
    recordedTriggers: arrayAt(record.recorded_triggers, `${path}.recorded_triggers`).map((item, index) => {
      const triggerPath = `${path}.recorded_triggers[${index}]`;
      const row = objectAt(item, triggerPath);
      exactKeysAt(row, ['sequence', 'event_id'], triggerPath);
      return {
        sequence: positiveIntegerAt(row.sequence, `${triggerPath}.sequence`),
        eventId: stringAt(row.event_id, `${triggerPath}.event_id`),
      };
    }),
    unrecordedTriggerCount: count('unrecorded_trigger_count'),
    retainedRows: count('retained_rows'),
    missingSequences: count('missing_sequences'),
    writerDeliveryLossBefore: count('writer_delivery_loss_before'),
    writerDeliveryLossDuring: count('writer_delivery_loss_during'),
    coverage,
    uncertainty,
    contents: 'retained_event_context',
  };
}

export function parseHistorianCapturePage(value: unknown): HistorianCapturePage {
  const path = 'history.captures';
  const record = objectAt(value, path);
  exactKeysAt(record, ['schema_version', 'captures', 'truncated', 'next_before_capture_id', 'retention'], path);
  if (record.schema_version !== 'manyforge.historian_capture_page.v1') {
    return fail(`${path}.schema_version`, 'manyforge.historian_capture_page.v1');
  }
  const captures = arrayAt(record.captures, `${path}.captures`);
  if (captures.length > 32) return fail(`${path}.captures`, 'at most 32 captures');
  const parsed = captures.map((item, index) => parseHistorianCapture(item, `${path}.captures[${index}]`));
  const truncated = booleanAt(record.truncated, `${path}.truncated`);
  const next = record.next_before_capture_id === null
    ? null : nonnegativeInt64TextAt(record.next_before_capture_id, `${path}.next_before_capture_id`);
  if (next !== (truncated ? parsed.at(-1)?.captureId ?? null : null)) {
    return fail(`${path}.next_before_capture_id`, 'the last returned capture when truncated');
  }
  const retention = parseHistorianRetention(record.retention);
  if (retention === null) return fail(`${path}.retention`, 'retention coverage');
  return {
    schemaVersion: record.schema_version,
    captures: parsed,
    truncated,
    nextBeforeCaptureId: next,
    retention,
  };
}

export function parseHistorianDiscovery(value: unknown): HistorianDiscovery {
  const record = objectAt(value, 'history.discovery');
  exactKeysAt(record, ['schema_version', 'kind', 'values', 'truncated', 'next_after'], 'history.discovery');
  if (record.schema_version !== 'manyforge.historian_discovery.v1') return fail('history.discovery.schema_version', 'manyforge.historian_discovery.v1');
  const kind = stringAt(record.kind, 'history.discovery.kind');
  if (kind !== 'program_run_id' && kind !== 'component' && kind !== 'channel' && kind !== 'event_type') return fail('history.discovery.kind', 'a declared discovery kind');
  const values = arrayAt(record.values, 'history.discovery.values').map((value) => stringAt(value, 'history.discovery.value'));
  if (values.length > 32 || new Set(values).size !== values.length || values.some((value) => !value || new TextEncoder().encode(value).length > 1024 || value !== value.normalize('NFC'))) return fail('history.discovery.values', 'at most 32 unique nonempty NFC values of at most 1024 bytes');
  const truncated = booleanAt(record.truncated, 'history.discovery.truncated');
  const nextAfter = record.next_after === null ? null : stringAt(record.next_after, 'history.discovery.next_after');
  if ((truncated && values.length === 0) || nextAfter !== (truncated ? values.at(-1) : null)) return fail('history.discovery.next_after', 'an advancing exact cursor');
  return { schemaVersion: record.schema_version, kind, values, truncated, nextAfter };
}

export function parseHistorianPage(value: unknown): HistorianPage {
  const record = objectAt(value, 'history');
  exactKeysAt(record, [
    'events',
    'next_before_sequence',
    'schema_version',
    'truncated',
    'time_window',
    'retention',
  ], 'history');
  const schemaVersion = stringAt(record.schema_version, 'history.schema_version');
  if (schemaVersion !== 'manyforge.historian_summary_page.v2') {
    return fail('history.schema_version', 'manyforge.historian_summary_page.v2');
  }
  const window = objectAt(record.time_window, 'history.time_window');
  exactKeysAt(window, ['start_utc_ns', 'end_utc_ns'], 'history.time_window');
  const events = arrayAt(record.events, 'history.events');
  if (events.length > 32) return fail('history.events', 'at most 32 summary rows');
  return {
    schemaVersion,
    events: events.map((value, index) => {
      const stored = objectAt(value, `history.events[${index}]`);
      exactKeysAt(stored, ['event', 'sequence'], `history.events[${index}]`);
      return {
        sequence: positiveIntegerAt(
          stored.sequence,
          `history.events[${index}].sequence`,
        ),
        event: parseHistorianEventSummary(
          stored.event,
          `history.events[${index}].event`,
        ),
      };
    }),
    nextBeforeSequence: record.next_before_sequence === null
      ? null
      : positiveIntegerAt(
          record.next_before_sequence,
          'history.next_before_sequence',
        ),
    truncated: booleanAt(record.truncated, 'history.truncated'),
    timeWindow: {
      startUtcNs: window.start_utc_ns === null ? null : nonnegativeInt64TextAt(window.start_utc_ns, 'history.time_window.start_utc_ns'),
      endUtcNs: window.end_utc_ns === null ? null : nonnegativeInt64TextAt(window.end_utc_ns, 'history.time_window.end_utc_ns'),
    },
    retention: parseHistorianRetention(record.retention),
  };
}

function historianDetailSectionAt(
  value: unknown,
  path: string,
): HistorianDetail['sections']['incident'] {
  const record = objectAt(value, path);
  exactKeysAt(record, ['available', 'reason', 'value'], path);
  const available = booleanAt(record.available, `${path}.available`);
  const reason = record.reason === null ? null : stringAt(record.reason, `${path}.reason`);
  if (available === (reason !== null) || available === (record.value === null)) {
    return fail(path, 'one consistent available/reason/value union');
  }
  return { available, reason, value: record.value };
}

export function parseHistorianDetail(value: unknown): HistorianDetail {
  const record = objectAt(value, 'historyDetail');
  exactKeysAt(record, ['event', 'schema_version', 'sections', 'sequence'], 'historyDetail');
  const schemaVersion = stringAt(record.schema_version, 'historyDetail.schema_version');
  if (schemaVersion !== 'manyforge.historian_detail.v2') {
    return fail('historyDetail.schema_version', 'manyforge.historian_detail.v2');
  }
  const sections = objectAt(record.sections, 'historyDetail.sections');
  exactKeysAt(
    sections,
    ['blackboard', 'execution_digest', 'incident', 'latency'],
    'historyDetail.sections',
  );
  return {
    schemaVersion,
    sequence: positiveIntegerAt(record.sequence, 'historyDetail.sequence'),
    event: parseHistorianEvent(record.event, 'historyDetail.event'),
    sections: {
      incident: historianDetailSectionAt(sections.incident, 'historyDetail.sections.incident'),
      executionDigest: historianDetailSectionAt(
        sections.execution_digest,
        'historyDetail.sections.execution_digest',
      ),
      blackboard: historianDetailSectionAt(
        sections.blackboard,
        'historyDetail.sections.blackboard',
      ),
      latency: historianDetailSectionAt(sections.latency, 'historyDetail.sections.latency'),
    },
  };
}
