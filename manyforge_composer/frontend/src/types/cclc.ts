export type CCLCLifecycleIntent = 'prepare' | 'enable' | 'disable' | 'recover';

export interface CCLCOwnerIdentity {
  requestOwnerEpoch: string;
}
export type CCLCLifecycleState =
  | 'undeclared'
  | 'declared'
  | 'prepared'
  | 'active'
  | 'degraded'
  | 'faulted'
  | 'disabling'
  | 'disabled';

export interface CCLCActiveConfigurationRef {
  id: string;
  generation: number;
}

export interface CCLCControlBinding {
  controlBindingId: string;
  controlBindingRevision: string;
  controlRouteId: string;
}

export interface CCLCComponentDomain {
  controlDomainId: string;
  controlDomainRevision: string;
  controllerTopologyId: string;
  topologyRevision: string;
  activeConfiguration: CCLCActiveConfigurationRef | null;
  currentControlBindings: CCLCControlBinding[];
  currentResources: string[];
}

export interface CCLCProviderOccurrence {
  controlDomainId: string;
  lifecyclePlanCatalogDigest: string;
  providerInstanceId: string;
  providerManifestId: string;
  providerManifestRevision: string;
  providerRuntimeEpoch: string;
  providerRuntimeUnitId: string;
}

export interface CCLCComponentSnapshot {
  componentId: string;
  lifecycleGeneration: number;
  lifecycleState: CCLCLifecycleState;
  retainedBundleId: string;
  graphRevision: string;
  domains: CCLCComponentDomain[];
  providers: CCLCProviderOccurrence[];
}

export interface CCLCExpectedActiveConfiguration {
  presence: 'absent' | 'present';
  active_control_configuration_id: string | null;
  active_control_configuration_generation: number | null;
}

export interface CCLCDesiredBindingRoute {
  control_binding_id: string;
  control_binding_revision: string;
  control_route_id: string;
}

export interface CCLCDesiredConfiguration {
  control_domain_id: string;
  control_domain_revision: string;
  controller_topology_id: string;
  topology_revision: string;
  activation_set_id: string;
  binding_routes: CCLCDesiredBindingRoute[];
}

export interface CCLCComponentExpectation {
  controllable_component_id: string;
  expected_component_lifecycle_generation: number;
  expected_component_lifecycle_state: 'prepared' | 'active' | 'disabled';
}

export interface CCLCRouteTemplate {
  desired_configuration: CCLCDesiredConfiguration;
  expected_active_control_configuration: CCLCExpectedActiveConfiguration;
  component_expectations: CCLCComponentExpectation[];
}

export interface CCLCRouteTemplateSet {
  componentId: string;
  retainedBundleId: string;
  graphRevision: string;
  templates: CCLCRouteTemplate[];
}

export interface CCLCRequestContext {
  request_id: string;
  request_owner_epoch: string;
  source_label: string;
  correlation_id: string | null;
}

export interface CCLCLifecycleRequestBody {
  request_context: CCLCRequestContext;
  controllable_component_id: string;
  intent: CCLCLifecycleIntent;
  expected_graph_revision: string;
  expected_component_lifecycle_generation: number;
  expected_admission_generation: number;
  expected_control_domains: Array<{
    control_domain_id: string;
    control_domain_revision: string;
    controller_topology_id: string;
    topology_revision: string;
    expected_active_control_configuration: CCLCExpectedActiveConfiguration;
    expected_control_bindings: Array<{
      control_binding_id: string;
      control_binding_revision: string;
      control_route_id: string;
    }>;
  }>;
  reason: string;
  confirmed: true;
}

export interface CCLCRouteRequestBody {
  request_context: CCLCRequestContext;
  expected_admission_generation: number;
  expected_retained_bundle_id: string;
  expected_graph_revision: string;
  desired_configuration: CCLCDesiredConfiguration;
  expected_active_control_configuration: CCLCExpectedActiveConfiguration;
  component_expectations: CCLCComponentExpectation[];
  confirmed: true;
}

export interface CCLCTransactionStatus {
  kind: 'lifecycle' | 'route';
  transactionId: string;
  phase: string;
  terminalResult:
    | 'succeeded'
    | 'failed_known_no_effect'
    | 'failed_partial_effect'
    | null;
  reconciliationRequired: boolean;
  error: {
    code: string;
    message: string;
    details: Record<string, string>;
  } | null;
}

export interface CCLCAdmission {
  replayStatus:
    | 'accepted_new'
    | 'replayed_exact'
    | 'request_result_expired'
    | 'request_id_conflict'
    | 'request_ledger_full'
    | 'stale_request_owner_epoch';
  requestId: string | null;
  transactionId: string | null;
  status: CCLCTransactionStatus | null;
}
