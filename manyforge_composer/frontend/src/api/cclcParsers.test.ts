import { describe, expect, it } from 'vitest';

import {
  parseCCLCAdmission,
  parseCCLCComponentSnapshot,
  parseCCLCOwnerIdentity,
  parseCCLCRouteTemplateSet,
  parseCCLCStatusEnvelope,
} from './cclcParsers';

function componentSnapshot(): Record<string, unknown> {
  return {
    active_public_pointer_digest: `sha256:${'1'.repeat(64)}`,
    component_lifecycle_generation: 4,
    component_lifecycle_state: 'prepared',
    controllable_component_id: 'component.arm',
    domains: [{
      active_control_configuration: null,
      control_domain_id: 'domain.arm',
      control_domain_revision: 'domain.revision.1',
      controller_topology_id: 'topology.arm',
      current_control_bindings: [{
        control_binding_id: 'binding.arm',
        control_binding_revision: 'binding.revision.1',
        control_route_id: 'route.normal',
      }],
      current_resources: ['joint.1'],
      topology_revision: 'topology.revision.1',
    }],
    graph_revision: 'graph.1',
    lifecycle_progress: {
      blocking_cause_class: 'availability',
      blocking_reason: 'controller_manager not ready',
      delayed: false,
      incident: null,
      next_intent: 'enable',
      phase: 'waiting_for_prerequisites',
      waiting_since_ns: 1_000,
    },
    lifecycle_target: {
      origin: 'startup',
      requested_state: 'active',
      target_generation: 1,
    },
    provider_occurrences: [{
      control_domain_id: 'domain.arm',
      lifecycle_plan_catalog_digest: `sha256:${'2'.repeat(64)}`,
      provider_instance_id: 'provider.arm',
      provider_manifest_id: 'manifest.arm',
      provider_manifest_revision: `sha256:${'3'.repeat(64)}`,
      provider_runtime_epoch: 'runtime.epoch.1',
      provider_runtime_unit_id: 'runtime.arm',
    }],
    publication_generation: 7,
    retained_bundle_id: 'bundle.1',
    snapshot_digest: `sha256:${'4'.repeat(64)}`,
  };
}

function lifecycleAcceptance(): Record<string, unknown> {
  return {
    accepted_at_ns: 100,
    component_lifecycle_generation: 5,
    lifecycle_transaction_generation: 2,
    lifecycle_transaction_id: 'lifecycle.2',
    request_id: 'request.2',
    timeout_duration_ns: 1_000_000,
  };
}

describe('CCLC closed response parsers', () => {
  it('parses only the sole CCLC owner identity field', () => {
    expect(parseCCLCOwnerIdentity({
      request_owner_epoch: 'cclc-owner.1',
    })).toEqual({ requestOwnerEpoch: 'cclc-owner.1' });
    expect(() => parseCCLCOwnerIdentity({
      request_owner_epoch: 'cclc-owner.1', extra: true,
    })).toThrow('closed fields');
  });

  it('projects one complete owner snapshot and rejects noncanonical scope', () => {
    const value = componentSnapshot();
    const parsed = parseCCLCComponentSnapshot(value);
    expect(parsed).toMatchObject({
      componentId: 'component.arm',
      lifecycleGeneration: 4,
      lifecycleState: 'prepared',
      graphRevision: 'graph.1',
      providers: [{
        controlDomainId: 'domain.arm',
        providerInstanceId: 'provider.arm',
        providerRuntimeUnitId: 'runtime.arm',
      }],
    });

    const torn = structuredClone(value);
    const domain = (torn.domains as Array<Record<string, unknown>>)[0]!;
    domain.current_resources = ['joint.2', 'joint.1'];
    expect(() => parseCCLCComponentSnapshot(torn)).toThrow('canonical');
  });

  it('projects the requested target and owner progress separately from the achieved state', () => {
    const parsed = parseCCLCComponentSnapshot(componentSnapshot());
    expect(parsed.lifecycleState).toBe('prepared');
    expect(parsed.lifecycleTarget).toEqual({
      requestedState: 'active', origin: 'startup', targetGeneration: 1,
    });
    expect(parsed.lifecycleProgress).toEqual({
      phase: 'waiting_for_prerequisites',
      nextIntent: 'enable',
      blockingCauseClass: 'availability',
      blockingReason: 'controller_manager not ready',
      waitingSinceNs: 1_000,
      delayed: false,
      incident: null,
    });

    const nulls = componentSnapshot();
    nulls.lifecycle_target = null;
    nulls.lifecycle_progress = null;
    const parsedNulls = parseCCLCComponentSnapshot(nulls);
    expect(parsedNulls.lifecycleTarget).toBeNull();
    expect(parsedNulls.lifecycleProgress).toBeNull();

    const nullableProgress = componentSnapshot();
    Object.assign(nullableProgress.lifecycle_progress as Record<string, unknown>, {
      blocking_cause_class: 'none',
      blocking_reason: null,
      next_intent: null,
      phase: 'satisfied',
      waiting_since_ns: null,
    });
    expect(parseCCLCComponentSnapshot(nullableProgress).lifecycleProgress).toMatchObject({
      phase: 'satisfied', nextIntent: null, blockingReason: null, waitingSinceNs: null,
    });
  });

  it('rejects missing, widened or unknown lifecycle target and progress facts', () => {
    for (const key of ['lifecycle_target', 'lifecycle_progress']) {
      const missing = componentSnapshot();
      delete missing[key];
      expect(() => parseCCLCComponentSnapshot(missing)).toThrow('closed fields');
    }

    const rejected: Array<[string, string, unknown, string]> = [
      ['lifecycle_target', 'requested_state', 'degraded', 'active, prepared or disabled'],
      ['lifecycle_target', 'origin', 'browser', 'startup, operator or program'],
      ['lifecycle_target', 'target_generation', 0, 'positive safe integer'],
      ['lifecycle_progress', 'phase', 'retrying', 'known lifecycle progress phase'],
      ['lifecycle_progress', 'next_intent', 'restart', 'known lifecycle intent'],
      ['lifecycle_progress', 'blocking_cause_class', 'estop', 'known blocking cause class'],
      ['lifecycle_progress', 'blocking_reason', '', 'bounded non-empty NFC text'],
      ['lifecycle_progress', 'delayed', 'yes', 'boolean'],
      ['lifecycle_progress', 'waiting_since_ns', -1, 'nanoseconds'],
    ];
    for (const [field, key, invalid, message] of rejected) {
      const value = componentSnapshot();
      (value[field] as Record<string, unknown>)[key] = invalid;
      expect(() => parseCCLCComponentSnapshot(value)).toThrow(message);
    }

    const widened = componentSnapshot();
    (widened.lifecycle_progress as Record<string, unknown>).acknowledged = true;
    expect(() => parseCCLCComponentSnapshot(widened)).toThrow('closed fields');
    const narrowed = componentSnapshot();
    delete (narrowed.lifecycle_target as Record<string, unknown>).origin;
    expect(() => parseCCLCComponentSnapshot(narrowed)).toThrow('closed fields');
  });

  it('projects an open incident with the new cause classes and rejects a malformed one', () => {
    const incident = (): Record<string, unknown> => ({
      acknowledged: false,
      cause_class: 'collision',
      detected_at_ns: 1_759_900_000_000,
      incident_id: 'incident.arm.1',
      summary: 'Collision detected while executing',
      vendor_codes: ['C31', 'state=4', 'mode=0'],
    });
    for (const cause of ['collision', 'external_control', 'provider_lost'] as const) {
      const value = componentSnapshot();
      Object.assign(value.lifecycle_progress as Record<string, unknown>, {
        blocking_cause_class: cause,
        phase: 'blocked',
        incident: { ...incident(), cause_class: cause },
      });
      const parsed = parseCCLCComponentSnapshot(value).lifecycleProgress;
      expect(parsed?.blockingCauseClass).toBe(cause);
      expect(parsed?.incident).toEqual({
        incidentId: 'incident.arm.1',
        causeClass: cause,
        vendorCodes: ['C31', 'state=4', 'mode=0'],
        detectedAtNs: 1_759_900_000_000,
        summary: 'Collision detected while executing',
        acknowledged: false,
      });
    }

    const rejected: Array<[string, unknown, string]> = [
      ['cause_class', 'availability', 'intervention cause class'],
      ['cause_class', 'none', 'intervention cause class'],
      ['incident_id', '', 'bounded non-empty NFC text'],
      ['incident_id', 'i'.repeat(129), 'bounded non-empty NFC text'],
      ['summary', 'Cafe\u0301', 'bounded non-empty NFC text'],
      ['summary', 's'.repeat(1025), 'bounded non-empty NFC text'],
      ['vendor_codes', 'C31', 'bounded vendor codes'],
      ['vendor_codes', [''], 'bounded vendor codes'],
      ['vendor_codes', Array(17).fill('C'), 'bounded vendor codes'],
      ['detected_at_ns', -1, 'nanoseconds'],
      ['acknowledged', true, 'false'],
    ];
    for (const [key, invalid, message] of rejected) {
      const value = componentSnapshot();
      (value.lifecycle_progress as Record<string, unknown>).incident = {
        ...incident(), [key]: invalid,
      };
      expect(() => parseCCLCComponentSnapshot(value)).toThrow(message);
    }
    const widened = componentSnapshot();
    (widened.lifecycle_progress as Record<string, unknown>).incident = {
      ...incident(), occurred: true,
    };
    expect(() => parseCCLCComponentSnapshot(widened)).toThrow('closed fields');
    const missing = componentSnapshot();
    delete (missing.lifecycle_progress as Record<string, unknown>).incident;
    expect(() => parseCCLCComponentSnapshot(missing)).toThrow('closed fields');
  });

  it('accepts only exact retained route templates', () => {
    const value = {
      controllable_component_id: 'component.arm',
      graph_revision: 'graph.1',
      retained_bundle_id: 'bundle.1',
      templates: [{
        component_expectations: [{
          controllable_component_id: 'component.arm',
          expected_component_lifecycle_generation: 4,
          expected_component_lifecycle_state: 'prepared',
        }],
        desired_configuration: {
          activation_set_id: 'activation.arm',
          binding_routes: [{
            control_binding_id: 'binding.arm',
            control_binding_revision: 'binding.revision.1',
            control_route_id: 'route.fast',
          }],
          control_domain_id: 'domain.arm',
          control_domain_revision: 'domain.revision.1',
          controller_topology_id: 'topology.arm',
          topology_revision: 'topology.revision.1',
        },
        expected_active_control_configuration: { presence: 'absent' },
      }],
    };
    expect(parseCCLCRouteTemplateSet(value).templates).toHaveLength(1);

    const disabled = structuredClone(value);
    disabled.templates[0].component_expectations[0].expected_component_lifecycle_state = 'disabled';
    expect(parseCCLCRouteTemplateSet(disabled).templates[0].component_expectations[0]
      .expected_component_lifecycle_state).toBe('disabled');
    disabled.templates[0].component_expectations[0].expected_component_lifecycle_state = 'faulted';
    expect(() => parseCCLCRouteTemplateSet(disabled)).toThrow('prepared, active or disabled');

    const widened = structuredClone(value);
    (widened.templates[0] as Record<string, unknown>).caller_override = true;
    expect(() => parseCCLCRouteTemplateSet(widened)).toThrow('closed fields');
  });

  it('joins admission and status to the exact accepted transaction', () => {
    const acceptance = lifecycleAcceptance();
    const status = {
      acceptance,
      accepted_at_ns: 100,
      controllable_component_id: 'component.arm',
      current_state: 'prepared',
      domains: [{
        action_state: 'not_started',
        control_domain_id: 'domain.arm',
        current_state: 'prepared',
        effect: 'none',
        error: null,
      }],
      error: null,
      latest_updated_at_ns: 110,
      phase: 'accepted',
      previous_state: 'declared',
      reconciliation_required: false,
      remaining_duration_ns: 999_990,
      status_owner_epoch: 'cclc.epoch.1',
      status_sequence: 1,
      terminal_result: null,
    };
    const result = parseCCLCAdmission({
      acceptance,
      replay_status: 'accepted_new',
      status,
    }, 'lifecycle');
    expect(result).toMatchObject({
      requestId: 'request.2',
      transactionId: 'lifecycle.2',
      status: { phase: 'accepted', reconciliationRequired: false },
    });
    expect(parseCCLCStatusEnvelope({ status }, 'lifecycle')?.transactionId)
      .toBe('lifecycle.2');

    const failed = {
      ...structuredClone(status),
      phase: 'failed',
      terminal_result: 'failed_known_no_effect',
      error: {
        code: 'native_lifecycle_route_unavailable',
        message: 'The native route did not become ready',
        details: { reason: 'route_readback_missing' },
      },
    };
    expect(parseCCLCStatusEnvelope({ status: failed }, 'lifecycle')?.error)
      .toEqual(failed.error);

    const mismatched = structuredClone(status);
    (mismatched.acceptance as Record<string, unknown>).lifecycle_transaction_id =
      'lifecycle.foreign';
    expect(() => parseCCLCAdmission({
      acceptance,
      replay_status: 'accepted_new',
      status: mismatched,
    }, 'lifecycle')).toThrow('accepted transaction status');
  });

  it('accepts the authorization-only Recover of an open incident and nothing looser', () => {
    const recovery = {
      acknowledged_at_ns: 9_000,
      acknowledged_by: 'manyforge_composer',
      cause_class: 'emergency_stop',
      controllable_component_id: 'component.arm',
      incident_id: 'incident.component.arm.5000',
    };
    for (const replayStatus of ['accepted_new', 'replayed_exact'] as const) {
      expect(parseCCLCAdmission({
        acceptance: null,
        incident_recovery: recovery,
        replay_status: replayStatus,
        status: null,
      }, 'lifecycle')).toEqual({
        replayStatus,
        requestId: null,
        transactionId: null,
        status: null,
        incidentRecovery: {
          incidentId: 'incident.component.arm.5000',
          componentId: 'component.arm',
          causeClass: 'emergency_stop',
          acknowledgedBy: 'manyforge_composer',
          acknowledgedAtNs: 9_000,
        },
      });
    }
    // An ordinary admission carries no recovery.
    expect(parseCCLCAdmission({
      acceptance: null, replay_status: 'stale_request_owner_epoch', status: null,
    }, 'lifecycle').incidentRecovery).toBeNull();
    // Only a lifecycle result can carry it, never with a transaction or refusal.
    expect(() => parseCCLCAdmission({
      acceptance: null, incident_recovery: recovery, replay_status: 'accepted_new', status: null,
    }, 'route')).toThrow('closed fields');
    expect(() => parseCCLCAdmission({
      acceptance: null,
      incident_recovery: recovery,
      replay_status: 'request_id_conflict',
      status: null,
    }, 'lifecycle')).toThrow('authorization-only');
    expect(() => parseCCLCAdmission({
      acceptance: null,
      incident_recovery: { ...recovery, cause_class: 'availability' },
      replay_status: 'accepted_new',
      status: null,
    }, 'lifecycle')).toThrow('cause_class');
    const extra = { ...recovery, summary: 'not in the contract' };
    expect(() => parseCCLCAdmission({
      acceptance: null, incident_recovery: extra, replay_status: 'accepted_new', status: null,
    }, 'lifecycle')).toThrow('closed fields');
  });
});
