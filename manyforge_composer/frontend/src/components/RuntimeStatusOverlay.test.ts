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
});
