import { describe, expect, it } from 'vitest';

import {
  componentLifecyclePresentation,
  componentLifecycleToneClass,
  componentReadinessOccurrencePresentation,
} from './componentLifecyclePresentation';
import type {
  CCLCComponentSnapshot,
  CCLCLifecycleProgress,
  CCLCLifecycleTarget,
  CCLCReadinessProviderOccurrence,
} from '../types/cclc';

function snapshot(
  lifecycleState: CCLCComponentSnapshot['lifecycleState'],
  target: Partial<CCLCLifecycleTarget> | null,
  progress: Partial<CCLCLifecycleProgress> | null,
): CCLCComponentSnapshot {
  return {
    controllableComponentId: 'component.arm',
    lifecycleGeneration: 4,
    lifecycleState,
    retainedBundleId: 'bundle.1',
    publicationGeneration: 1, activePublicPointerDigest: 'pointer.1',
    graphRevision: 'graph.1',
    domains: [],
    providers: [],
    readiness: { requestOwnerEpoch: 'owner.1', evaluatedAtNs: 100,
      nextIntent: lifecycleState === 'declared' || lifecycleState === 'disabled' ? 'prepare' : lifecycleState === 'prepared' ? 'enable' : null,
      requestedState: lifecycleState === 'declared' || lifecycleState === 'disabled' ? 'prepared' : lifecycleState === 'prepared' ? 'active' : null,
      ready: lifecycleState === 'active',
      error: lifecycleState === 'active' ? null : { code: 'not_ready', message: 'controller_manager not ready', details: {} },
      providerOccurrences: [], measuredState: { required: false, available: null, error: null } },
    lifecycleTarget: target === null ? null : {
      requestedState: 'active', origin: 'startup', targetGeneration: 1, ...target,
    },
    lifecycleProgress: progress === null ? null : {
      phase: 'waiting_for_prerequisites',
      nextIntent: 'prepare',
      blockingCauseClass: 'availability',
      blockingReason: null,
      waitingSinceNs: 1_000,
      delayed: false,
      incident: null,
      ...progress,
    },
  };
}

function message(value: CCLCComponentSnapshot): string {
  return componentLifecyclePresentation(value).message;
}

function readinessDomain(): CCLCReadinessProviderOccurrence {
  return {
    providerOccurrence: { controlDomainId: 'domain.gripper', controlDomainRevision: 'revision.1',
      lifecyclePlanCatalogDigest: 'catalog.1', providerInstanceId: 'provider.1',
      providerManifestId: 'manifest.1', providerManifestRevision: 'revision.1',
      providerRuntimeEpoch: 'epoch.1', providerRuntimeUnitId: 'unit.1' },
    health: { observedState: 'active', diagnostic: null, facts: [
      { lifecycleFactId: 'external.controller_loaded', value: { kind: 'boolean', booleanValue: true } },
      { lifecycleFactId: 'external.controller_configuration_ready', value: { kind: 'boolean', booleanValue: true } },
      { lifecycleFactId: 'external.controller_inactive_unrequested', value: { kind: 'boolean', booleanValue: false } },
      { lifecycleFactId: 'external.fault_present', value: { kind: 'boolean', booleanValue: true } },
      { lifecycleFactId: 'external.transport_ready', value: { kind: 'boolean', booleanValue: false } },
    ] }, decisionKind: null, error: null,
  };
}

describe('domain activation presentation', () => {
  it.each([undefined, true])('preserves existing presentation for activation fact %s', (hasActivationStep) => {
    const domain = readinessDomain();
    domain.hasActivationStep = hasActivationStep;
    expect(componentReadinessOccurrencePresentation(domain)).toEqual({ state: 'active', facts: domain.health!.facts });
  });
  it('words no activation step without displaying normalized controller facts or changing raw health', () => {
    const domain = readinessDomain();
    domain.hasActivationStep = false;
    const presented = componentReadinessOccurrencePresentation(domain);
    expect(presented.state).toBe('No activation step');
    expect(presented.facts.map((fact) => fact.lifecycleFactId)).toEqual([
      'external.fault_present', 'external.transport_ready',
    ]);
    expect(domain.health!.observedState).toBe('active');
    expect(domain.health!.facts).toHaveLength(5);
  });
  it('retains genuine faulted and unavailable health alongside no activation step', () => {
    const domain = readinessDomain();
    domain.hasActivationStep = false;
    domain.health!.observedState = 'faulted';
    expect(componentReadinessOccurrencePresentation(domain).state).toBe('No activation step · faulted');
    domain.health = null;
    expect(componentReadinessOccurrencePresentation(domain).state).toBe('No activation step · health unavailable');
  });
});

describe('component lifecycle presentation', () => {
  it('presents the four separate facts for a startup target that is waiting', () => {
    const presented = componentLifecyclePresentation(snapshot('declared', {}, {
      blockingReason: 'controller_manager not ready',
    }));
    expect(presented).toEqual({
      message: 'Waiting for controller',
      tone: 'waiting',
      target: 'active (requested by startup configuration, target 1)',
      achieved: 'declared · generation 4',
      readiness: 'Not ready · [not_ready] controller_manager not ready · next step: prepare · Target wait duration unavailable',
      consequence: 'Motion is unavailable until the component reaches its requested target.',
      incident: null,
    });
  });

  it('uses only the owner-issued delayed flag for slower-than-expected startup', () => {
    expect(message(snapshot('declared', {}, { delayed: true }))).toBe(
      'Startup delayed — still waiting',
    );
    expect(message(snapshot('prepared', {}, {
      phase: 'transition_in_progress', blockingCauseClass: 'none', nextIntent: 'enable',
    }))).toBe('Lifecycle step in progress: enable');
  });

  it('reports an uncertain command outcome while the owner reconciles', () => {
    const presented = componentLifecyclePresentation(snapshot('prepared', {}, {
      phase: 'outcome_uncertain', blockingCauseClass: 'none',
    }));
    expect(presented.message).toBe('Activation outcome uncertain — checking');
    expect(presented.tone).toBe('held');
  });

  it('distinguishes controller loss after Active from waiting before it', () => {
    const lost = componentLifecyclePresentation(snapshot('active', {}, {
      phase: 'blocked', nextIntent: null,
    }));
    expect(lost.message).toBe('Controller lost — motion held');
    expect(lost.tone).toBe('held');
    expect(lost.consequence).toContain('interrupted work stays held');
    expect(message(snapshot('prepared', {}, {}))).toBe('Waiting for controller');
    expect(message(snapshot('active', { requestedState: 'prepared' }, {}))).toBe(
      'Waiting for controller',
    );
  });

  it.each([
    ['emergency_stop', 'Emergency stop engaged'],
    ['protective_stop', 'Protective stop engaged'],
    ['safeguard_stop', 'Safeguard stop engaged'],
    ['fault', 'Fault — intervention required'],
    ['configuration', 'Configuration mismatch'],
    ['unknown', 'Controller status unknown — intervention required'],
  ] as const)('requires intervention for %s', (cause, expected) => {
    const presented = componentLifecyclePresentation(snapshot('active', {}, {
      phase: 'blocked', blockingCauseClass: cause, delayed: true,
    }));
    expect(presented.message).toBe(expected);
    expect(presented.tone).toBe('intervention');
    expect(presented.consequence).toContain('never clears faults or enables drives');
  });

  it.each([
    ['collision', 'Collision detected — acknowledgement required'],
    ['external_control', 'Robot moved from its own interface — acknowledgement required'],
    ['provider_lost', 'Controller lost — incident, motion held'],
    ['emergency_stop', 'Emergency stop — acknowledgement required'],
  ] as const)('an open %s incident takes precedence and shows its facts', (cause, expected) => {
    const presented = componentLifecyclePresentation(snapshot('active', {
      requestedState: 'disabled', origin: 'operator',
    }, {
      phase: 'blocked',
      // The incident wins even over a different current blocking cause.
      blockingCauseClass: 'none',
      nextIntent: 'recover',
      incident: {
        incidentId: 'incident.arm.1',
        causeClass: cause,
        vendorCodes: ['C31', 'state=4'],
        detectedAtNs: 1_000,
        summary: 'Unexpected change after Active',
        acknowledged: false,
      },
    }));
    expect(presented.message).toBe(expected);
    expect(presented.tone).toBe('intervention');
    expect(presented.incident).toContain('Unexpected change after Active');
    expect(presented.incident).toContain('vendor codes: C31, state=4');
    expect(presented.consequence).toContain('Recover cell acknowledges the incident');
    expect(presented.consequence).toContain('Motion never resumes by itself');
  });

  it.each([
    ['collision', 'Collision detected — intervention required'],
    ['external_control', 'Robot moved from its own interface — intervention required'],
    ['provider_lost', 'Controller lost — motion held'],
  ] as const)('words the new %s blocking cause without an incident', (cause, expected) => {
    expect(message(snapshot('active', {}, {
      phase: 'blocked', blockingCauseClass: cause,
    }))).toBe(expected);
  });

  it('attributes a Disable to the operator only when the operator requested it', () => {
    const operator = snapshot('disabled', {
      requestedState: 'disabled', origin: 'operator', targetGeneration: 3,
    }, { phase: 'satisfied', blockingCauseClass: 'none', nextIntent: null });
    expect(componentLifecyclePresentation(operator)).toMatchObject({
      message: 'Disabled by operator',
      target: 'disabled (requested by operator, target 3)',
    });
    expect(message(snapshot('disabled', {
      requestedState: 'disabled', origin: 'startup',
    }, { phase: 'satisfied', blockingCauseClass: 'none', nextIntent: null }))).toBe(
      'Not ready',
    );
  });

  it('is Ready only when the achieved state is Active and progress is satisfied', () => {
    const ready = componentLifecyclePresentation(snapshot('active', {}, {
      phase: 'satisfied', blockingCauseClass: 'none', nextIntent: null,
    }));
    expect(ready.message).toBe('Ready');
    expect(ready.tone).toBe('ready');
    expect(message(snapshot('prepared', { requestedState: 'prepared' }, {
      phase: 'satisfied', blockingCauseClass: 'none', nextIntent: null,
    }))).toBe('Not ready');
  });

  it('presents absent owner target facts without inventing progress', () => {
    const presented = componentLifecyclePresentation(snapshot('prepared', null, null));
    expect(presented.target).toBe('No requested target');
    expect(presented.readiness).toBe('Not ready · [not_ready] controller_manager not ready · next step: enable · Target wait duration unavailable');
    expect(presented.message).toBe('Not ready');
    expect(message(snapshot('active', null, null))).toBe('Ready');
    expect(message(snapshot('prepared', {}, {
      phase: 'cancelled', blockingCauseClass: 'none', nextIntent: null,
    }))).toBe('Lifecycle target cancelled');
  });

  it('reuses the shared status-pill classes', () => {
    expect(componentLifecycleToneClass('ready')).toBe('status-pill status-pill-connected');
    expect(componentLifecycleToneClass('waiting')).toBe('status-pill');
    expect(componentLifecycleToneClass('held')).toBe('status-pill status-pill-danger');
    expect(componentLifecycleToneClass('intervention')).toBe(
      'status-pill phase-intervention_required',
    );
  });

  it('presents an owner-retired target separately from an ordinary cancellation', () => {
    const retired = componentLifecyclePresentation(snapshot('prepared', null, {
      phase: 'cancelled', blockingCauseClass: 'none', nextIntent: null,
      blockingReason: 'target_retired:original_work_settled',
    }));
    expect(retired.message).toBe('Target retired');
    expect(retired.consequence).toContain('original_work_settled');
    expect(retired.achieved).toBe('prepared · generation 4');
    expect(message(snapshot('prepared', null, { phase: 'cancelled', blockingCauseClass: 'none',
      nextIntent: null, blockingReason: 'operator cancelled request' }))).toBe('Lifecycle target cancelled');
  });
});

it('shows actual degraded readiness even for durable Active and keeps a real health fault visible', () => {
  const current = snapshot('active', null, null);
  current.readiness = { ...current.readiness, ready: false,
    error: { code: 'measured_source_unavailable', message: 'Measured feed is stale', details: {} } };
  expect(componentLifecyclePresentation(current).message).toBe('Not ready');
  expect(componentLifecyclePresentation(current).readiness).toContain('Measured feed is stale');
  current.readiness.providerOccurrences = [{ providerOccurrence: {
    controlDomainId: 'domain.arm', controlDomainRevision: 'revision.1', providerInstanceId: 'provider.1',
    providerManifestId: 'manifest.1', providerManifestRevision: 'manifest.revision.1', providerRuntimeUnitId: 'runtime.1',
    providerRuntimeEpoch: 'epoch.1', lifecyclePlanCatalogDigest: 'catalog.1' },
    health: { observedState: 'faulted', facts: [], diagnostic: null }, decisionKind: null, error: current.readiness.error }];
  expect(componentLifecyclePresentation(current).message).toBe('Fault — intervention required');
});


it('shows exact owner target-wait elapsed and preserves code, message and cause', () => {
  const current = snapshot('declared', {}, {});
  current.readiness.evaluatedAtNs = '9007199257240993';
  current.lifecycleProgress!.waitingSinceNs = '9007199254740993';
  current.readiness.error = { code: 'measured_state_unavailable', message: 'Actual measured read failed', details: { reason: 'actual_source_stale' } };
  const before = structuredClone(current);
  expect(componentLifecyclePresentation(current).readiness).toBe('Not ready · [measured_state_unavailable] Actual measured read failed · actual_source_stale · next step: prepare · Target wait: 0.002 s at last owner evaluation');
  expect(current).toEqual(before);
});
it('does not invent elapsed time for missing, future or satisfied owner waiting facts', () => {
  const current = snapshot('declared', {}, {});
  for (const waiting of [null, 101]) {
    current.lifecycleProgress!.waitingSinceNs = waiting;
    expect(componentLifecyclePresentation(current).readiness).toContain('Target wait duration unavailable');
  }
  current.lifecycleProgress!.waitingSinceNs = 100;
  expect(componentLifecyclePresentation(current).readiness).toContain('Target wait: 0.000 s at last owner evaluation');
  for (const phase of ['satisfied', 'cancelled'] as const) {
    current.lifecycleProgress!.phase = phase;
    expect(componentLifecyclePresentation(current).readiness).toContain('Target wait duration unavailable');
  }
  current.readiness.ready = true;
  expect(componentLifecyclePresentation(current).readiness).not.toContain('Target wait');
});
