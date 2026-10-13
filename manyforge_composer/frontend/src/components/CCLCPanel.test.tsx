/// <reference types="node" />

import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';

import { CCLCPanelView } from './CCLCPanel';
import type { CCLCController } from '../hooks/useCCLC';
import type { CCLCReadinessDomain } from '../types/cclc';

function controller(): CCLCController {
  return {
    componentId: 'component.arm',
    snapshot: {
      componentId: 'component.arm',
      lifecycleGeneration: 4,
      lifecycleState: 'prepared',
      retainedBundleId: 'bundle.1',
    publicationGeneration: 1, activePublicPointerDigest: 'pointer.1',
      graphRevision: 'graph.1',
      domains: [],
      providers: [],
      lifecycleTarget: null,
      lifecycleProgress: null,
      readiness: { requestOwnerEpoch: 'owner.1', evaluatedAtNs: 100, nextIntent: 'enable', requestedState: 'active',
        ready: true, error: null, domains: [], measuredState: { required: false, available: null, error: null } },
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
    activationRefusal: null, activationUnresolved: false, componentHealth: {},
    submitLifecycle: vi.fn(),
    submitRoute: vi.fn(),
  };
}

describe('CCLC presentation', () => {
  it('renders no activation step while preserving faults, transport and refusal diagnostics', () => {
    const current = controller();
    const domain: CCLCReadinessDomain = {
      hasActivationStep: false,
      providerOccurrence: { controlDomainId: 'domain.gripper', controlDomainRevision: 'revision.1',
        lifecyclePlanCatalogDigest: 'catalog.1', providerInstanceId: 'provider.1',
        providerManifestId: 'manifest.1', providerManifestRevision: 'revision.1',
        providerRuntimeEpoch: 'epoch.1', providerRuntimeUnitId: 'unit.1' },
      health: { observedState: 'faulted', diagnostic: { code: 'fault', message: 'Actual transport fault', details: {} },
        facts: [
          { lifecycleFactId: 'external.controller_loaded', value: { kind: 'boolean', booleanValue: true } },
          { lifecycleFactId: 'external.fault_present', value: { kind: 'boolean', booleanValue: true } },
          { lifecycleFactId: 'external.transport_ready', value: { kind: 'boolean', booleanValue: false } },
        ] }, decisionKind: null, error: { code: 'not_ready', message: 'Transport unavailable', details: {} },
    };
    current.snapshot!.readiness = { ...current.snapshot!.readiness, ready: false,
      error: domain.error, domains: [domain] };
    const html = renderToStaticMarkup(createElement(CCLCPanelView, { enabled: true,
      mutationAvailable: true, mutationUnavailableReason: null, controller: current }));
    expect(html).toContain('No activation step · faulted');
    expect(html).toContain('Fault — intervention required');
    expect(html).toContain('Actual transport fault');
    expect(html).toContain('Transport unavailable');
    expect(html).toContain('external.fault_present: true');
    expect(html).toContain('external.transport_ready: false');
    expect(html).not.toContain('external.controller_loaded');
    expect(current.snapshot!.readiness.ready).toBe(false);
  });
  it('keeps Cancel available during readiness waiting and guards accepted unfinished work', () => {
    const waiting = controller();
    waiting.busy = true; waiting.activationProgress = 'component.arm: Not ready — [missing_reference] Missing measured feed · actual_joint_missing';
    waiting.snapshot!.readiness = { ...waiting.snapshot!.readiness, ready: false,
      error: { code: 'missing_reference', message: 'Missing measured feed', details: {} } };
    let html = renderToStaticMarkup(createElement(CCLCPanelView, { enabled: true,
      mutationAvailable: true, mutationUnavailableReason: null, controller: waiting }));
    expect(html).toContain('Cancel remaining activation');
    expect(html).toContain('component.arm: Not ready — [missing_reference] Missing measured feed · actual_joint_missing');
    expect(html).not.toMatch(/disabled=""[^>]*>Cancel remaining activation/);
    waiting.busy = false; waiting.activationUnresolved = true;
    html = renderToStaticMarkup(createElement(CCLCPanelView, { enabled: true,
      mutationAvailable: true, mutationUnavailableReason: null, controller: waiting }));
    expect(html).toMatch(/disabled=""[^>]*>Activate<\/button>/);
  });
  it('shows literal refusal facts and whole-current-list Retry without switching selection', () => {
    const failed = controller();
    failed.activationProgress = 'Activation stopped. Submit a new explicit command after resolving the cause.';
    failed.activationRefusal = { componentId: 'component.other', transactionId: 'prepare.1', error: {
      code: 'component_lifecycle_transition_missing', message: 'No transition satisfied', details: {
        reason: 'no_transition_satisfied', observed_state: 'declared', observed_controller_loaded: 'unknown',
        observed_fault_present: 'true', observed_transport_ready: 'nonboolean' } } };
    const html = renderToStaticMarkup(createElement(CCLCPanelView, { enabled: true,
      mutationAvailable: true, mutationUnavailableReason: null, controller: failed, componentIds: ['component.other', 'component.arm'] }));
    expect(html).toContain('Retry activation sequence');
    expect(html).toContain('component.other: Fault');
    expect(html).toContain('observed_controller_loaded: unknown');
    expect(html).toContain('observed_transport_ready: nonboolean');
    expect(failed.setComponentId).not.toHaveBeenCalled();
  });
  it('retains the exact unknown activation request address for read-only reconciliation', () => {
    const unknown = controller();
    unknown.activationUnresolved = true;
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
      readiness: { ...waiting.snapshot!.readiness, ready: false,
        error: { code: 'component_unavailable', message: 'controller_manager not ready', details: {} } },
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


it('renders actual readiness code/reason and owner elapsed without changing action eligibility', () => {
  const current = controller();
  current.snapshot!.readiness = { ...current.snapshot!.readiness, ready: false, evaluatedAtNs: '9007199257240993000',
    error: { code: 'owner_read_unavailable', message: 'Actual owner read failed', details: { reason: 'actual_binding_changed' } },
    measuredState: { required: true, available: false, error: { code: 'measured_state_stale', message: 'Measured sample too old', details: { reason: 'joint_age_exceeded' } } } };
  current.snapshot!.lifecycleProgress = { phase: 'waiting_for_prerequisites', nextIntent: 'enable', blockingCauseClass: 'availability',
    blockingReason: 'actual_binding_changed', waitingSinceNs: '9007199254740993000', delayed: false, incident: null };
  const html = renderToStaticMarkup(createElement(CCLCPanelView, { enabled: true, mutationAvailable: true, mutationUnavailableReason: null, controller: current }));
  expect(html).toContain('[owner_read_unavailable] Actual owner read failed · actual_binding_changed');
  expect(html).toContain('[measured_state_stale] Measured sample too old · joint_age_exceeded');
  expect(html).toContain('Target wait: 2.500 s at last owner evaluation');
  expect(html).not.toMatch(/<button[^>]*disabled=""[^>]*>Enable<\/button>/);
});


it('preserves activation-domain health diagnostic code and cause independently of the plan error', () => {
  const current = controller();
  current.snapshot!.readiness.domains = [{ hasActivationStep: true,
    providerOccurrence: { controlDomainId: 'domain.arm', controlDomainRevision: 'revision.1', lifecyclePlanCatalogDigest: 'catalog.1',
      providerInstanceId: 'provider.arm', providerManifestId: 'manifest.arm', providerManifestRevision: 'revision.1',
      providerRuntimeEpoch: 'epoch.1', providerRuntimeUnitId: 'unit.1' },
    health: { observedState: 'faulted', facts: [], diagnostic: { code: 'actual_vendor_fault', message: 'Actual arm transport fault', details: { reason: 'actual_fault_bit' } } },
    decisionKind: null, error: null }];
  const html = renderToStaticMarkup(createElement(CCLCPanelView, { enabled: true, mutationAvailable: true, mutationUnavailableReason: null, controller: current }));
  expect(html).toContain('[actual_vendor_fault] Actual arm transport fault · actual_fault_bit');
});


it('renders an identical intervention plan error and health diagnostic once', () => {
  const current = controller();
  const error = { code: 'actual_vendor_fault', message: 'Actual arm transport fault', details: { reason: 'actual_fault_bit' } };
  const domain: CCLCReadinessDomain = { hasActivationStep: true,
    providerOccurrence: { controlDomainId: 'domain.arm', controlDomainRevision: 'revision.1', lifecyclePlanCatalogDigest: 'catalog.1',
      providerInstanceId: 'provider.arm', providerManifestId: 'manifest.arm', providerManifestRevision: 'revision.1',
      providerRuntimeEpoch: 'epoch.1', providerRuntimeUnitId: 'unit.1' },
    error, decisionKind: null,
    health: { observedState: 'faulted', facts: [], diagnostic: structuredClone(error) } };
  current.snapshot!.readiness = { ...current.snapshot!.readiness, ready: false, domains: [domain] };
  const html = renderToStaticMarkup(createElement(CCLCPanelView, { enabled: true, mutationAvailable: true, mutationUnavailableReason: null, controller: current }));
  expect(html.split('[actual_vendor_fault] Actual arm transport fault · actual_fault_bit')).toHaveLength(2);
});
