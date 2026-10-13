import type { Nanoseconds } from './nanoseconds';

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

export type CCLCLifecycleTargetState = 'active' | 'prepared' | 'disabled';
export type CCLCLifecycleTargetOrigin = 'startup' | 'operator' | 'program';

/** The requested lifecycle target CCLC follows; distinct from the achieved state. */
export interface CCLCLifecycleTarget {
  requestedState: CCLCLifecycleTargetState;
  origin: CCLCLifecycleTargetOrigin;
  targetGeneration: number;
}

export type CCLCLifecycleProgressPhase =
  | 'satisfied'
  | 'waiting_for_prerequisites'
  | 'transition_in_progress'
  | 'outcome_uncertain'
  | 'blocked'
  | 'cancelled';

export type CCLCBlockingCauseClass =
  | 'none'
  | 'availability'
  | 'emergency_stop'
  | 'protective_stop'
  | 'safeguard_stop'
  | 'fault'
  | 'configuration'
  | 'unknown'
  | 'collision'
  | 'external_control'
  | 'provider_lost';

/** Cause of an open incident; never `none` or `availability`. */
export type CCLCIncidentCauseClass = Exclude<CCLCBlockingCauseClass, 'none' | 'availability'>;

/** Exact observed uint64 wire scalar; this carries no runtime authority. */
export type CCLCUnsigned64 = number | string;

/** Stored evidence retains its wire field names for faithful readback. */
export interface CCLCIncidentDiagnosticSnapshot {
  exit: {
    status: number | null;
    signal: number | null;
    core_dumped: boolean;
    observed_utc: string;
  };
  context: {
    component_id: string | null;
    request_context: string | null;
    runtime_unit_id: string | null;
    runtime_epoch: string | null;
    bundle_id: string | null;
    graph_revision: string | null;
    generator_identity: string | null;
    launch_identity: string | null;
  };
  readiness: {
    known: boolean | null;
    controller_manager_present: boolean | null;
    controller_observed: boolean | null;
    controller_loaded: boolean | null;
    controller_bound: boolean | null;
    arm_controller_active: boolean | null;
    hardware_ready: boolean | null;
    joint_sample_available: boolean | null;
    binding_epoch: CCLCUnsigned64 | null;
    sample_age_ns: Nanoseconds | null;
    captured_utc: string | null;
  };
  settings: {
    argv: string[];
    controller_profile: string | null;
    runtime_namespace: string | null;
    arm_controller_name: string | null;
    gripper_controller_name: string | null;
    frame_prefix: string | null;
    controllers_yaml: string | null;
    /** Effective settings text; the client does not interpret configuration. */
    retained_configuration_json: string | null;
  };
  generated: {
    robot_description_sha256: string | null;
    controllers_sha256: string | null;
  };
  output: {
    tail: string;
    line_count: number;
    truncated: boolean;
    dropped_forward_lines: CCLCUnsigned64;
    dropped_forward_bytes: CCLCUnsigned64;
    invalid_text_bytes: CCLCUnsigned64;
  };
  core_dump_link: string | null;
}

export interface CCLCIncidentDiagnostic {
  error: NonNullable<CCLCTransactionStatus['error']>;
  snapshot: CCLCIncidentDiagnosticSnapshot | null;
}

/**
 * An unexpected change after the component reached its target. It stays
 * open until the operator's `recover` acknowledgement is accepted; the full
 * record stays in history.
 */
export interface CCLCComponentIncident {
  incidentId: string;
  causeClass: CCLCIncidentCauseClass;
  vendorCodes: string[];
  detectedAtNs: Nanoseconds;
  summary: string;
  acknowledged: false;
  diagnostic?: CCLCIncidentDiagnostic | null;
}

/** CCLC-issued progress toward the requested target; presentation only. */
export interface CCLCLifecycleProgress {
  phase: CCLCLifecycleProgressPhase;
  nextIntent: CCLCLifecycleIntent | null;
  blockingCauseClass: CCLCBlockingCauseClass;
  blockingReason: string | null;
  waitingSinceNs: Nanoseconds | null;
  delayed: boolean;
  incident: CCLCComponentIncident | null;
}

export interface CCLCComponentSnapshot {
  componentId: string;
  lifecycleGeneration: number;
  lifecycleState: CCLCLifecycleState;
  retainedBundleId: string;
  graphRevision: string;
  domains: CCLCComponentDomain[];
  providers: CCLCProviderOccurrence[];
  lifecycleTarget: CCLCLifecycleTarget | null;
  lifecycleProgress: CCLCLifecycleProgress | null;
  /** Informational host output losses; unavailable observations stay absent. */
  droppedOutputLines?: CCLCUnsigned64;
  droppedOutputBytes?: CCLCUnsigned64;
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
  /**
   * Present only when CCLC answered the operator's `recover` of an open
   * incident authorization-only: the incident is closed without a lifecycle
   * transaction (`transactionId` and `status` are null) and keeps the retained evidence. Component activation is a separate
   * explicit operator action.
   */
  incidentRecovery: CCLCIncidentRecovery | null;
}

/** The operator's accepted acknowledgement of one open component incident. */
export interface CCLCIncidentRecovery {
  incidentId: string;
  componentId: string;
  causeClass: CCLCIncidentCauseClass;
  acknowledgedBy: string;
  acknowledgedAtNs: Nanoseconds;
}
