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

    const mismatched = structuredClone(status);
    (mismatched.acceptance as Record<string, unknown>).lifecycle_transaction_id =
      'lifecycle.foreign';
    expect(() => parseCCLCAdmission({
      acceptance,
      replay_status: 'accepted_new',
      status: mismatched,
    }, 'lifecycle')).toThrow('accepted transaction status');
  });
});
