/// <reference types="node" />

import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';

import type { OperatorAlert } from '../types/api';
import { HeaderBar } from './HeaderBar';

const operatorAlertState = vi.hoisted(() => ({
  alerts: [] as OperatorAlert[],
  blockingCount: 0,
  unacknowledgedAlertIds: [] as string[],
  generation: 0,
  timestampNs: 0,
  truncated: false,
  loaded: false,
  busy: false,
  error: null as string | null,
  acknowledge: vi.fn(),
  acknowledgeAll: vi.fn(),
  clearError: vi.fn(),
}));

const cycleState = vi.hoisted(() => ({
  state: 'idle',
  error: 'Duplicated cycle wrapper',
  errorCode: 'cycle_error',
  errorDetails: {} as Record<string, unknown>,
}));

vi.mock('../store/operatorAlertStore', () => ({
  useOperatorAlertStore: () => operatorAlertState,
  useOperatorAlertPolling: () => undefined,
}));

vi.mock('../store/cycleStore', () => ({
  useCycleStore: (selector?: (state: typeof cycleState) => unknown) => (
    selector ? selector(cycleState) : cycleState
  ),
  useEditLocked: () => false,
}));

function alert(index: number): OperatorAlert {
  return {
    schemaVersion: 'manyforge.operator_alert.v1',
    alertId: `alert-${index}`,
    recoveryContextId: `context-${index}`,
    incidentId: `incident-${index}`,
    incidentIds: [`incident-${index}`],
    executionId: index,
    severity: 'ERROR',
    errorCode: `test_error_${index}`,
    message: `Server alert ${index}`,
    errorDetails: {},
    firstSeenAtNs: index,
    lastSeenAtNs: index,
    occurrenceCount: 1,
    revision: 1,
  };
}

describe('HeaderBar operator alert stack', () => {
  it('shows three newest panels, total count, and acknowledge-all control', () => {
    Object.assign(operatorAlertState, {
      alerts: [alert(4), alert(3), alert(2)],
      blockingCount: 4,
      unacknowledgedAlertIds: ['alert-4', 'alert-3', 'alert-2', 'alert-1'],
      generation: 4,
      timestampNs: 4,
      truncated: true,
      loaded: true,
    });

    const html = renderToStaticMarkup(createElement(HeaderBar));
    expect(html).toContain('4 unacknowledged error alerts');
    expect(html).toContain('Showing newest 3');
    expect(html).toContain('Acknowledge all');
    expect(html).toContain('Server alert 4');
    expect(html).toContain('Server alert 3');
    expect(html).toContain('Server alert 2');
    expect(html).not.toContain('Server alert 1');
    expect(html).not.toContain('Duplicated cycle wrapper');
  });
});
