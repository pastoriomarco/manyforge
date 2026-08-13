import { describe, expect, it } from 'vitest';

import {
  lifecycleRequestFromSnapshot,
  routeRequestFromTemplate,
} from './useCCLC';
import type {
  CCLCComponentSnapshot,
  CCLCRequestContext,
  CCLCRouteTemplateSet,
} from '../types/cclc';

const context: CCLCRequestContext = {
  request_id: 'request.1',
  request_owner_epoch: 'composer.epoch.1',
  source_label: 'manyforge_composer',
  correlation_id: null,
};

const snapshot: CCLCComponentSnapshot = {
  componentId: 'component.arm',
  lifecycleGeneration: 4,
  lifecycleState: 'prepared',
  retainedBundleId: 'bundle.1',
  graphRevision: 'graph.1',
  domains: [{
    controlDomainId: 'domain.arm',
    controlDomainRevision: 'domain.revision.1',
    controllerTopologyId: 'topology.arm',
    topologyRevision: 'topology.revision.1',
    activeConfiguration: { id: 'active.1', generation: 7 },
    currentControlBindings: [{
      controlBindingId: 'binding.arm',
      controlBindingRevision: 'binding.revision.1',
      controlRouteId: 'route.normal',
    }],
    currentResources: ['joint.1'],
  }],
};

const templates: CCLCRouteTemplateSet = {
  componentId: 'component.arm',
  retainedBundleId: 'bundle.1',
  graphRevision: 'graph.1',
  templates: [{
    desired_configuration: {
      control_domain_id: 'domain.arm',
      control_domain_revision: 'domain.revision.1',
      controller_topology_id: 'topology.arm',
      topology_revision: 'topology.revision.1',
      activation_set_id: 'activation.arm',
      binding_routes: [{
        control_binding_id: 'binding.arm',
        control_binding_revision: 'binding.revision.1',
        control_route_id: 'route.fast',
      }],
    },
    expected_active_control_configuration: {
      presence: 'present',
      active_control_configuration_id: 'active.1',
      active_control_configuration_generation: 7,
    },
    component_expectations: [{
      controllable_component_id: 'component.arm',
      expected_component_lifecycle_generation: 4,
      expected_component_lifecycle_state: 'prepared',
    }],
  }],
};

describe('CCLC request projection', () => {
  it('copies lifecycle compare-and-set truth from the owner snapshot', () => {
    expect(lifecycleRequestFromSnapshot(snapshot, 'enable', context)).toEqual({
      request_context: context,
      controllable_component_id: 'component.arm',
      intent: 'enable',
      expected_graph_revision: 'graph.1',
      expected_component_lifecycle_generation: 4,
      expected_control_domains: [{
        control_domain_id: 'domain.arm',
        control_domain_revision: 'domain.revision.1',
        controller_topology_id: 'topology.arm',
        topology_revision: 'topology.revision.1',
        expected_active_control_configuration: {
          presence: 'present',
          active_control_configuration_id: 'active.1',
          active_control_configuration_generation: 7,
        },
        expected_control_bindings: [{
          control_binding_id: 'binding.arm',
          control_binding_revision: 'binding.revision.1',
          control_route_id: 'route.normal',
        }],
      }],
      reason: 'operator-confirmed Composer request',
      confirmed: true,
    });
  });

  it('copies the exact selected retained route template', () => {
    const request = routeRequestFromTemplate(templates, templates.templates[0]!, context);
    expect(request.desired_configuration).toBe(
      templates.templates[0]!.desired_configuration,
    );
    expect(request.component_expectations).toBe(
      templates.templates[0]!.component_expectations,
    );
    expect(request.confirmed).toBe(true);
    expect(() => routeRequestFromTemplate(
      templates,
      { ...templates.templates[0]! },
      context,
    )).toThrow('current projection');
  });
});
