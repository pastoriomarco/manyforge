import { describe, expect, it } from 'vitest';

import {
  componentLifecyclePresentation,
  componentLifecycleToneClass,
} from './componentLifecyclePresentation';
import type {
  CCLCComponentSnapshot,
  CCLCLifecycleProgress,
  CCLCLifecycleTarget,
} from '../types/cclc';

function snapshot(
  lifecycleState: CCLCComponentSnapshot['lifecycleState'],
  target: Partial<CCLCLifecycleTarget> | null,
  progress: Partial<CCLCLifecycleProgress> | null,
): CCLCComponentSnapshot {
  return {
    componentId: 'component.arm',
    lifecycleGeneration: 4,
    lifecycleState,
    retainedBundleId: 'bundle.1',
    graphRevision: 'graph.1',
    domains: [],
    providers: [],
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
      readiness: 'controller unavailable or not yet ready · controller_manager not ready · next step: prepare',
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
      'Component disabled',
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
    }))).toBe('Component prepared');
  });

  it('presents absent owner target facts without inventing progress', () => {
    const presented = componentLifecyclePresentation(snapshot('prepared', null, null));
    expect(presented.target).toBe('No requested target');
    expect(presented.readiness).toBe('Not reported by the lifecycle owner');
    expect(presented.message).toBe('Component prepared');
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
});
