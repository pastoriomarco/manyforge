import { describe, expect, it } from 'vitest';

import { captureEventPageFor } from '../components/FailureCapturePanel';
import { historianListPath } from './client';
import { parseHistorianCapturePage, parseHistorianPage } from './parsers';

function retention() {
  const partition = (name: string) => ({
    partition: name,
    retained_count: '3',
    retained_bytes: '300',
    pruned_count: '0',
    pruned_bytes: '0',
    last_pruned_sequence: null,
    unknown_time_count: '0',
    regressed_time_count: '0',
    loss_counter_saturated: false,
    first_recorded_utc_ns: null,
    last_recorded_utc_ns: null,
  });
  return {
    schema_version: 'manyforge.historian_retention.v2',
    partitions: [partition('routine'), partition('important')],
    captures: {
      live_count: '1',
      open_count: '0',
      protected_count: '6',
      protected_bytes: '600',
      live_capture_limit: '64',
      protected_byte_limit: '33554432',
      retired_count: '2',
      released_rows: '12',
      released_bytes: '1200',
      last_retired_capture_id: '2',
    },
    partition_row_limit: '100000',
    partition_record_byte_limit: '16777216',
    database_page_byte_limit: '536870912',
    duration_guaranteed: false,
    prior_pruning_known: true,
  };
}

function capture(overrides: Record<string, unknown> = {}) {
  return {
    capture_id: '3',
    state: 'closed',
    close_reason: 'tail_rows',
    closure_recorded: true,
    first_sequence: 4,
    trigger_sequence: 7,
    last_sequence: 9,
    trigger_event_id: 'event.7',
    trigger: {
      event_type: 'execute_failed',
      component: 'manyforge.runtime',
      condition_severity: 'ERROR',
      message: 'controller rejected trajectory',
    },
    opened_at_utc_ns: '1788516000000000000',
    preceding_rows: 3,
    preceding_limit: 'rows',
    tail_rows: 2,
    capture_bytes: '600',
    trigger_count: 1,
    recorded_triggers: [{ sequence: 7, event_id: 'event.7' }],
    unrecorded_trigger_count: 0,
    retained_rows: 6,
    missing_sequences: 0,
    writer_delivery_loss_before: 0,
    writer_delivery_loss_during: 0,
    coverage: 'complete',
    uncertainty: [],
    contents: 'retained_event_context',
    ...overrides,
  };
}

describe('failure capture contracts', () => {
  it('parses a bounded capture page with its retention budget', () => {
    const page = parseHistorianCapturePage({
      schema_version: 'manyforge.historian_capture_page.v1',
      captures: [capture()],
      truncated: false,
      next_before_capture_id: null,
      retention: retention(),
    });
    expect(page.captures[0]).toMatchObject({
      captureId: '3',
      state: 'closed',
      triggerSequence: 7,
      coverage: 'complete',
      trigger: { message: 'controller rejected trajectory' },
    });
    expect(page.retention.captures).toMatchObject({
      protectedCount: '6', retiredCount: '2', lastRetiredCaptureId: '2',
    });
  });

  it('rejects coverage claims that contradict their stated uncertainty', () => {
    const page = (item: Record<string, unknown>) => ({
      schema_version: 'manyforge.historian_capture_page.v1',
      captures: [item],
      truncated: false,
      next_before_capture_id: null,
      retention: retention(),
    });
    expect(() => parseHistorianCapturePage(page(capture({
      coverage: 'complete', uncertainty: ['tail_interrupted_by_restart'],
    })))).toThrow();
    expect(() => parseHistorianCapturePage(page(capture({
      coverage: 'uncertain', uncertainty: [],
    })))).toThrow();
    expect(() => parseHistorianCapturePage(page(capture({ contents: 'complete_state_dump' })))).toThrow();
    expect(() => parseHistorianCapturePage(page(capture({ trigger_sequence: 10 })))).toThrow();
    expect(parseHistorianCapturePage(page(capture({
      state: 'closed_by_restart', close_reason: 'restart', coverage: 'uncertain',
      uncertainty: ['tail_interrupted_by_restart', 'writer_delivery_loss_during_capture'],
    }))).captures[0].uncertainty).toHaveLength(2);
  });

  it('requires the exact continuation of a truncated page', () => {
    const wire = {
      schema_version: 'manyforge.historian_capture_page.v1',
      captures: [capture()],
      truncated: true,
      next_before_capture_id: '2',
      retention: retention(),
    };
    expect(() => parseHistorianCapturePage(wire)).toThrow();
    expect(parseHistorianCapturePage({ ...wire, next_before_capture_id: '3' }).nextBeforeCaptureId).toBe('3');
  });

  it('accepts the v2 retention profile on history pages', () => {
    expect(parseHistorianPage({
      schema_version: 'manyforge.historian_summary_page.v2',
      events: [],
      next_before_sequence: null,
      truncated: false,
      time_window: { start_utc_ns: null, end_utc_ns: null },
      retention: retention(),
    }).retention?.captures.liveCount).toBe('1');
  });

  it('restricts a history query to one positive capture id', () => {
    expect(historianListPath({ captureId: 3 })).toContain('capture_id=3');
    expect(() => historianListPath({ captureId: 0 })).toThrow();
    expect(() => historianListPath({ captureId: 1.5 })).toThrow();
  });

  it('shows capture events only under the capture they were read for', () => {
    const page = { captureId: '3', events: [], olderCursor: 7, showingOlder: false };
    expect(captureEventPageFor(page, '3')).toBe(page);
    // Selecting another capture (or its read failing) never shows capture 3's events.
    expect(captureEventPageFor(page, '4')).toBeNull();
    expect(captureEventPageFor(null, '3')).toBeNull();
  });
});
