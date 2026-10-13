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
      lifecycleTarget: null,
      lifecycleProgress: null,
    },
    routeTemplates: {
      componentId: 'component.arm',
      retainedBundleId: 'bundle.1',
      graphRevision: 'graph.1',
      templates: [],
    },
    transaction: null,
    pendingTransaction: null,
    pendingActivationRequest: null,
    busy: false,
    readUnavailableReason: null,
    error: null,
    setComponentId: vi.fn(),
    refresh: vi.fn(),
    reconcileTransaction: vi.fn(),
    dismissError: vi.fn(),
    activate: vi.fn(async () => {}),
    activateSequence: vi.fn(async () => {}),
    cancelActivation: vi.fn(),
    activationProgress: null,
    submitLifecycle: vi.fn(),
    submitRoute: vi.fn(),
  };
}

describe('CCLC presentation', () => {
  it('retains the exact unknown activation request address for read-only reconciliation', () => {
    const unknown = controller();
    unknown.pendingActivationRequest = {
      request_context: { request_id: 'sent.request.7', request_owner_epoch: 'owner.1',
        source_label: 'manyforge_composer', correlation_id: null },
      controllable_component_id: 'component.other', intent: 'prepare',
      expected_graph_revision: 'graph.1', expected_component_lifecycle_generation: 4,
      expected_admission_generation: 7, expected_control_domains: [], confirmed: true,
      reason: 'operator-confirmed Composer request',
    };
    const html = renderToStaticMarkup(createElement(CCLCPanelView, {
      enabled: true, mutationAvailable: true, mutationUnavailableReason: null,
      controller: unknown,
    }));
    expect(html).toContain('Activation outcome unknown for component.other');
    expect(html).toContain('sent.request.7');
    expect(html).toContain('Reconcile');
    expect(html).toMatch(/disabled=""[^>]*>Activate<\/button>/);
  });
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

  it('shows target, achieved state, readiness and consequence while manual controls stay available', () => {
    const waiting = controller();
    waiting.snapshot = {
      ...waiting.snapshot!,
      lifecycleTarget: { requestedState: 'active', origin: 'startup', targetGeneration: 2 },
      lifecycleProgress: {
        phase: 'waiting_for_prerequisites',
        nextIntent: 'enable',
        blockingCauseClass: 'availability',
        blockingReason: 'controller_manager not ready',
        waitingSinceNs: 1_000,
        delayed: true,
        incident: null,
      },
    };
    const html = renderToStaticMarkup(createElement(CCLCPanelView, {
      enabled: true,
      mutationAvailable: true,
      mutationUnavailableReason: null,
      controller: waiting,
    }));
    expect(html).toContain('Startup delayed — still waiting');
    expect(html).toContain('Requested target: active (requested by startup configuration, target 2)');
    expect(html).toContain('Achieved: prepared · generation 4');
    expect(html).toContain('controller_manager not ready');
    expect(html).toContain('Motion is unavailable until the component reaches its requested target.');
    expect(html).toContain('aria-label="Component lifecycle status"');
    expect(html).not.toMatch(/acknowledge|keep watching|resume watching/i);
    expect(html).toMatch(/<button[^>]*title="Enable the prepared component[^"]*"[^>]*>Enable<\/button>/);
    expect(html).not.toMatch(/<button[^>]*disabled=""[^>]*>Enable<\/button>/);
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
