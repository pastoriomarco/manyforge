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
    expect(html).toContain('Prepare');
    expect(html).toContain('Enable');
    expect(html).toContain('Disable');
    expect(html).not.toContain('Recover / restart');
    expect(html).not.toContain('Stop / disable');
    expect(html).toContain('Owner-issued state');
  });

  it('keeps read unavailability local to the lifecycle panel', () => {
    const unavailable = controller();
    unavailable.snapshot = null;
    unavailable.routeTemplates = null;
    unavailable.readUnavailableReason = 'Current component state is unavailable.';
    const html = renderToStaticMarkup(createElement(CCLCPanelView, {
      enabled: true,
      mutationAvailable: true,
      mutationUnavailableReason: null,
      controller: unavailable,
    }));
    expect(html).toContain('Current component state is unavailable.');
    expect(html).not.toContain('error-state');
  });

  it('shows an owner-issued failure phase and reason without treating it as an operator Disable', () => {
    const failed = controller();
    failed.pendingTransaction = { kind: 'lifecycle', id: 'lifecycle.7' };
    failed.transaction = {
      kind: 'lifecycle', transactionId: 'lifecycle.7', phase: 'failed',
      terminalResult: 'failed_known_no_effect', reconciliationRequired: false,
      error: {
        code: 'native_lifecycle_route_unavailable',
        message: 'The native route did not become ready',
        details: { reason: 'route_readback_missing' },
      },
    };
    const html = renderToStaticMarkup(createElement(CCLCPanelView, {
      enabled: true,
      mutationAvailable: false,
      mutationUnavailableReason: 'The component is not ready.',
      controller: failed,
    }));
    expect(html).toContain('native_lifecycle_route_unavailable');
    expect(html).toContain('The native route did not become ready');
    expect(html).toContain('route_readback_missing');
  });
});
