/// <reference types="node" />

import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';
import { OperatorAlarmPanel } from './OperatorAlarmPanel';

describe('operator alarm presentation', () => {
  it('renders adapter-supplied lifecycle copy and exact-record action context', () => {
    const html = renderToStaticMarkup(createElement(OperatorAlarmPanel, {
      alarm: {
        conditionId: 'condition-1',
        activationId: 'activation-2',
        recordRevision: 3,
        definitionId: 'manyforge.test.alarm',
        lifecycleLabel: 'ACTIVE — ACKNOWLEDGEMENT REQUIRED',
        priorityLabel: 'HIGH PRIORITY',
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
    expect(html).toContain('ACTIVE — ACKNOWLEDGEMENT REQUIRED');
    expect(html).toContain('activation-2');
    expect(html).not.toContain('forged-activation');
    expect(html).not.toContain('999');
    expect(html).toContain('severity-warning');
    expect(html).toContain(
      'data-presentation-key="condition-1:activation-2"',
    );
    expect(html).toContain('record revision');
    expect(html).toContain('does not clear the condition or resume motion');
  });
});
