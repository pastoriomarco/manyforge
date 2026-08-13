/// <reference types="node" />

import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';

import { CCLCPanelView } from './CCLCPanel';
import type { CCLCController } from '../hooks/useCCLC';

function controller(): CCLCController {
  return {
    componentId: 'component.arm',
    snapshot: {
      componentId: 'component.arm',
      lifecycleGeneration: 4,
      lifecycleState: 'prepared',
      retainedBundleId: 'bundle.1',
      graphRevision: 'graph.1',
      domains: [],
    },
    routeTemplates: {
      componentId: 'component.arm',
      retainedBundleId: 'bundle.1',
      graphRevision: 'graph.1',
      templates: [],
    },
    transaction: null,
    pendingTransaction: null,
    confirmed: false,
    busy: false,
    error: null,
    setComponentId: vi.fn(),
    setConfirmed: vi.fn(),
    refresh: vi.fn(),
    submitLifecycle: vi.fn(),
    submitRoute: vi.fn(),
  };
}

describe('CCLC presentation', () => {
  it('shows owner facts while every mutation requires confirmation', () => {
    const html = renderToStaticMarkup(createElement(CCLCPanelView, {
      enabled: true,
      controller: controller(),
    }));
    expect(html).toContain('component.arm');
    expect(html).toContain('prepared');
    expect(html).toContain('generation 4');
    expect(html).toContain('Explicit confirmation');
    expect(html).toContain('disabled=""');
    expect(html).toContain('No client-side lifecycle state');
  });
});
