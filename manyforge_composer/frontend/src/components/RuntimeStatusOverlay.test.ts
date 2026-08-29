/// <reference types="node" />

import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';
import { RuntimeStatusOverlay } from './RuntimeStatusOverlay';

describe('runtime status presentation shell', () => {
  it('renders unavailable state without inventing readiness', () => {
    const html = renderToStaticMarkup(createElement(RuntimeStatusOverlay, {
      status: null,
      backendConnected: false,
      error: 'Headless status is not qualified.',
      connectionLabel: 'Headless projection unavailable',
      connectionDetail: 'Headless status is not qualified.',
    }));
    expect(html).toContain('readiness-offline');
    expect(html).toContain('Headless status is not qualified.');
  });

  it('uses only adapter-supplied summary and readiness fields', () => {
    const html = renderToStaticMarkup(createElement(RuntimeStatusOverlay, {
      status: {
        readiness: 'degraded',
        summaryLabel: 'Partial',
        profileLabel: 'native',
        authorityLabel: 'manager-observed',
        userMessage: 'One optional component is unavailable.',
        capabilities: ['render'],
        components: [],
      },
      backendConnected: true,
      connectionLabel: 'Headless owner connected',
      connectionDetail: 'Current projection',
    }));
    expect(html).toContain('readiness-degraded');
    expect(html).toContain('Warn');
  });

  it('counts only actionable alarm and Composer attention', () => {
    const html = renderToStaticMarkup(createElement(RuntimeStatusOverlay, {
      status: {
        readiness: 'degraded',
        summaryLabel: 'Attention required',
        profileLabel: 'native',
        authorityLabel: 'source-owned facts',
        userMessage: 'One client response requires review.',
        capabilities: [],
        components: [],
      },
      backendConnected: true,
      connectionLabel: 'Headless owner connected',
      connectionDetail: 'Current projection',
      unacknowledgedAlarms: 1,
      activeAcknowledgedAlarms: 1,
      clientErrorCount: 1,
    }));

    expect(html).toContain('runtime-status-count">2');
  });
});
