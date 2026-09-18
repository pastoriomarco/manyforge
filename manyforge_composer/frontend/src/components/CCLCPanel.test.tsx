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
      providers: [],
    },
    routeTemplates: {
      componentId: 'component.arm',
      retainedBundleId: 'bundle.1',
      graphRevision: 'graph.1',
      templates: [],
    },
    transaction: null,
    pendingTransaction: null,
    busy: false,
    readUnavailableReason: null,
    error: null,
    setComponentId: vi.fn(),
    refresh: vi.fn(),
    reconcileTransaction: vi.fn(),
    dismissError: vi.fn(),
    submitLifecycle: vi.fn(),
    submitRoute: vi.fn(),
  };
}

describe('CCLC presentation', () => {
  it('shows owner facts while every mutation requires confirmation', () => {
    const html = renderToStaticMarkup(createElement(CCLCPanelView, {
      enabled: true,
      mutationAvailable: true,
      mutationUnavailableReason: null,
      controller: controller(),
    }));
    expect(html).toContain('component.arm');
    expect(html).toContain('prepared');
    expect(html).toContain('generation 4');
    expect(html).toContain('Start / prepare');
    expect(html).toContain('Connect / enable');
    expect(html).toContain('Owner-issued state');
  });

  it('keeps read unavailability local to the lifecycle panel', () => {
    const unavailable = controller();
    unavailable.snapshot = null;
    unavailable.routeTemplates = null;
    unavailable.readUnavailableReason = 'This Deployment uses direct native control.';
    const html = renderToStaticMarkup(createElement(CCLCPanelView, {
      enabled: true,
      mutationAvailable: true,
      mutationUnavailableReason: null,
      controller: unavailable,
    }));
    expect(html).toContain('This Deployment uses direct native control.');
    expect(html).not.toContain('error-state');
  });
});
