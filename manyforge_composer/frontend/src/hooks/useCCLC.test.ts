import { afterEach, describe, expect, it, vi } from 'vitest';

import {
  browserRequestIdentity,
  cclcRequestOwnerEpoch,
  lifecycleSupportsRouteSelection,
  lifecycleRequestFromSnapshot,
  routeRequestFromTemplate,
} from './useCCLC';
import type {
  CCLCComponentSnapshot,
  CCLCRequestContext,
  CCLCRouteTemplateSet,
} from '../types/cclc';
import type { ExecutionIdentity } from '../types/robotControl';

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
  providers: [],
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

const execution = {
  ownerEpoch: 'kernel-owner.1',
  cellId: 'cell.1',
  activePublicationGeneration: 1,
  controlManagementMode: 'component_lifecycle',
  graphRevision: snapshot.graphRevision,
  retainedBundleId: snapshot.retainedBundleId,
  sceneRevision: {
    scene_id: 'scene.1',
    revision_id: 1,
    content_hash: 'sha256:scene',
  },
  programId: 'program.1',
  programRevision: 'sha256:program',
  programBindingId: 'sha256:binding',
  motionTargetRoleId: 'arm',
  gripperTargetRoleId: 'gripper',
  gripperCommandSchemaId: 'gripper.command_position',
  gripperCommandSchemaRevision: 'sha256:gripper',
} satisfies ExecutionIdentity;

afterEach(() => {
  vi.restoreAllMocks();
  vi.unstubAllGlobals();
});

describe('CCLC request projection', () => {
  it('targets the CCLC owner epoch and rejects torn product projections', () => {
    const owner = { requestOwnerEpoch: 'cclc-owner.1' };
    expect(cclcRequestOwnerEpoch(owner, snapshot, execution)).toBe('cclc-owner.1');
    expect(() => cclcRequestOwnerEpoch(owner, snapshot, {
      ...execution,
      graphRevision: 'graph.2',
    })).toThrow('projections are torn');
  });

  it('reads retained routes only in route-capable lifecycle states', () => {
    expect(lifecycleSupportsRouteSelection('declared')).toBe(false);
    expect(lifecycleSupportsRouteSelection('prepared')).toBe(true);
    expect(lifecycleSupportsRouteSelection('active')).toBe(true);
    expect(lifecycleSupportsRouteSelection('disabled')).toBe(false);
  });

  it('creates bounded identities when randomUUID is unavailable on trusted-LAN HTTP', () => {
    vi.stubGlobal('crypto', {
      getRandomValues: (words: Uint32Array) => {
        words.set([1, 2, 3, 4]);
        return words;
      },
    });

    expect(browserRequestIdentity('composer-test')).toBe(
      'composer-test-00000001000000020000000300000004',
    );
  });

  it('keeps the last-resort identity fallback unique without Web Crypto', () => {
    vi.stubGlobal('crypto', undefined);
    vi.spyOn(Date, 'now').mockReturnValue(1_787_247_600_000);

    const first = browserRequestIdentity('composer-test');
    const second = browserRequestIdentity('composer-test');
    expect(first).not.toBe(second);
    expect(first).toMatch(/^composer-test-[0-9a-z]+-[0-9a-z]+$/);
    expect(second).toMatch(/^composer-test-[0-9a-z]+-[0-9a-z]+$/);
  });

  it('copies lifecycle compare-and-set truth from the owner snapshot', () => {
    expect(lifecycleRequestFromSnapshot(snapshot, 'enable', context, 7)).toEqual({
      request_context: context,
      controllable_component_id: 'component.arm',
      intent: 'enable',
      expected_graph_revision: 'graph.1',
      expected_component_lifecycle_generation: 4,
      expected_admission_generation: 7,
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
    const request = routeRequestFromTemplate(
      templates, templates.templates[0]!, context, 7,
    );
    expect(request.desired_configuration).toBe(
      templates.templates[0]!.desired_configuration,
    );
    expect(request.component_expectations).toBe(
      templates.templates[0]!.component_expectations,
    );
    expect(request.expected_admission_generation).toBe(7);
    expect(request.confirmed).toBe(true);
    expect(() => routeRequestFromTemplate(
      templates,
      { ...templates.templates[0]! },
      context,
      7,
    )).toThrow('current projection');
  });
});
