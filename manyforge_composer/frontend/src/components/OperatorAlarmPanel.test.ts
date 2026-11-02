/// <reference types="node" />

import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';
import { OperatorAlarmPanel, operatorAlarmCause, operatorAlarmStatus } from './OperatorAlarmPanel';

describe('operator alarm presentation', () => {
  it('renders adapter-supplied lifecycle copy and exact-record action context', () => {
    const html = renderToStaticMarkup(createElement(OperatorAlarmPanel, {
      alarm: {
        conditionId: 'condition-1',
        activationId: 'activation-2',
        recordRevision: 3,
        definitionId: 'manyforge.test.alarm',
        lifecycleLabel: 'Active · Acknowledgement required',
        priorityLabel: 'High priority',
        severity: 'warning',
        title: 'Test alarm',
        message: 'A condition requires attention.',
        operatorResponse: 'Inspect the observed condition.',
        details: {
          activation_id: 'forged-activation',
          record_revision: 999,
        },
      },
      onAcknowledge: () => undefined,
      onMinimize: () => undefined,
    }));
    expect(html).toContain('Active · Acknowledgement required');
    expect(html).toContain('activation-2');
    expect(html).not.toContain('forged-activation');
    expect(html).not.toContain('999');
    expect(html).toContain('severity-warning');
    expect(html).toContain(
      'data-presentation-key="condition-1:activation-2"',
    );
    expect(html).toContain('record revision');
    expect(html).toContain('does not clear the condition or resume motion');
    expect(html).toContain('<details>');
    expect(html).not.toContain('<details open="">');
  });

  it('heads with the cause, then the category, node and lifecycle in one status line', () => {
    const alarm = {
      conditionId: 'condition-1',
      activationId: 'activation-2',
      recordRevision: 3,
      definitionId: 'manyforge.program.operation_failure',
      lifecycleLabel: 'Active · Acknowledgement required',
      priorityLabel: 'High priority',
      severity: 'error' as const,
      title: 'Program operation failed',
      message: 'Program operation failed',
      operatorResponse: 'Review the original operation failure.',
      details: {
        failure: { code: 'ik_failed', message: 'RoboPlan could not resolve the pose goal' },
        node_instance_id: 'unreachable_hold',
        occurrence_count: 1,
      },
    };
    const cause = 'RoboPlan could not resolve the pose goal';
    const status = 'Program operation failed · node unreachable_hold · Active · '
      + 'Acknowledgement required · High priority · 1 occurrence';
    expect(operatorAlarmCause(alarm)).toBe(cause);
    expect(operatorAlarmStatus(alarm)).toBe(status);
    const html = renderToStaticMarkup(createElement(OperatorAlarmPanel, { alarm }));
    expect(html).toContain(`<summary>${cause} · ${status}</summary>`);
    expect(html).toContain(`<h2>${cause}</h2><p class="kernel-error-status">${status}</p>`);
    expect(html).toContain('<p>Review the original operation failure.</p>');
    // The full details table stays, collapsed under Technical details.
    expect(html).toMatch(/<details class="kernel-error-technical"><summary>Technical details<\/summary>.*node instance id/);
    // Without a failure message the alarm message is the cause.
    expect(operatorAlarmCause({ ...alarm, details: { failure: { message: '' } } }))
      .toBe('Program operation failed');
  });
});
