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

/** Existing Core provider-domain proof, kept verbatim (wire casing) so a request can echo it. */
export interface CCLCProviderControlDomainWire {
  controllable_component_id: string;
  provider_runtime_unit_id: string;
  provider_instance_id: string;
  provider_manifest_id: string;
  provider_manifest_revision: string;
  lifecycle_plan_catalog_digest: string;
  signal_configuration_digest: string;
  signal_names: string[];
  lifecycle_command_timeout_ns: number;
}

/** Members every response domain carries; `kind` gives `resources` its meaning. */
interface CCLCComponentDomainBase {
  kind: 'controller' | 'io_provider';
  controlDomainId: string;
  controlDomainRevision: string;
  /**
   * Core resource_ids(): controller = resources of the current control binding;
   * io_provider = sealed logical outputs (empty for input-only providers).
   */
  resources: string[];
  activeConfiguration: CCLCActiveConfigurationRef | null;
}

/** Model-free I/O provider domain: no topology, bindings or physical resources. */
export interface CCLCProviderComponentDomain extends CCLCComponentDomainBase {
  kind: 'io_provider';
  provider: CCLCProviderControlDomainWire;
  providerRuntimeEpoch: string;
}

export type CCLCComponentDomain = CCLCControllerComponentDomain | CCLCProviderComponentDomain;

export interface CCLCControllerComponentDomain extends CCLCComponentDomainBase {
  kind: 'controller';
  controllerTopologyId: string;
  topologyRevision: string;
  currentControlBindings: CCLCControlBinding[];
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
export interface CCLCIncidentDiagnosticSnapshotWire {
  exit: {
    status: number | null;
    signal: number | null;
    core_dumped: boolean;
    observed_utc: string;
  };
  context: {
    controllable_component_id: string | null;
    request_context: string | null;
    runtime_unit_id: string | null;
    runtime_epoch: string | null;
    retained_bundle_id: string | null;
    graph_revision: string | null;
    generator_identity: string | null;
    launch_identity: string | null;
  };
  /** ros2_control launch facts (null for an I/O provider); not the CCLC readiness verdict. */
  launch_readiness: {
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
  snapshot: CCLCIncidentDiagnosticSnapshotWire | null;
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
  controllableComponentId: string;
  lifecycleGeneration: number;
  lifecycleState: CCLCLifecycleState;
  retainedBundleId: string;
  graphRevision: string;
  publicationGeneration: number;
  activePublicPointerDigest: string;
  domains: CCLCComponentDomain[];
  providers: CCLCProviderOccurrence[];
  lifecycleTarget: CCLCLifecycleTarget | null;
  lifecycleProgress: CCLCLifecycleProgress | null;
  readiness: CCLCComponentReadiness;
  /** Informational host output losses; unavailable observations stay absent. */
  droppedOutputLines?: CCLCUnsigned64;
  droppedOutputBytes?: CCLCUnsigned64;
  /** CP9 health of the component's declared streams; absent when none. */
  streamHealth?: CCLCStreamHealth[];
}

export type CCLCStreamState = 'live' | 'stale' | 'absent';
export type CCLCStreamSeverity = 'fault' | 'blocking' | 'warning';
export type CCLCStreamRecovery = 'ack' | 'auto';
export type CCLCStreamTimestampSource = 'message_stamp' | 'arrival';

export interface CCLCStreamHealth {
  streamId: string;
  state: CCLCStreamState;
  /** Host-clock age of the last advancing sample; null when none arrived. */
  ageNs: number | null;
  budgetNs: number;
  severity: CCLCStreamSeverity;
  recovery: CCLCStreamRecovery;
  timestampSource: CCLCStreamTimestampSource;
  acknowledgementRequired: boolean;
  error: CCLCTransactionStatus['error'];
}

export type CCLCLifecycleFactValue =
  | { kind: 'unknown' }
  | { kind: 'boolean'; booleanValue: boolean }
  | { kind: 'integer'; integerValue: number }
  | { kind: 'token'; tokenValue: string };

/** One readiness row per provider occurrence (not per control domain). */
export interface CCLCReadinessProviderOccurrence {
  providerOccurrence: CCLCProviderOccurrence & { controlDomainRevision: string };
  /** Build-selected presentation fact; absent means not stated. */
  hasActivationStep?: boolean;
  health: {
    observedState: CCLCLifecycleState;
    facts: Array<{ lifecycleFactId: string; value: CCLCLifecycleFactValue }>;
    diagnostic: CCLCTransactionStatus['error'];
  } | null;
  decisionKind: 'already_satisfied' | 'dispatch_primitive' | null;
  error: CCLCTransactionStatus['error'];
}

/** Current owner verdict only; no readiness/admission authority or browser deadline. */
export interface CCLCComponentReadiness {
  requestOwnerEpoch: string;
  evaluatedAtNs: Nanoseconds;
  nextIntent: 'prepare' | 'enable' | null;
  requestedState: 'prepared' | 'active' | null;
  ready: boolean;
  error: CCLCTransactionStatus['error'];
  /** Wire `readiness.domains`: one row per provider occurrence. */
  providerOccurrences: CCLCReadinessProviderOccurrence[];
  measuredState: {
    required: boolean | null;
    available: boolean | null;
    error: CCLCTransactionStatus['error'];
  };
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

export interface CCLCRouteTemplateWire {
  desired_configuration: CCLCDesiredConfiguration;
  expected_active_control_configuration: CCLCExpectedActiveConfiguration;
  component_expectations: CCLCComponentExpectation[];
}

export interface CCLCRouteTemplateSet {
  controllableComponentId: string;
  retainedBundleId: string;
  graphRevision: string;
  templates: CCLCRouteTemplateWire[];
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
    kind: 'controller';
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
  } | {
    kind: 'io_provider';
    control_domain_id: string;
    control_domain_revision: string;
    provider: CCLCProviderControlDomainWire;
    expected_active_control_configuration: CCLCExpectedActiveConfiguration;
  }>;
  reason: string;
  /** Close acknowledges exactly this open incident; ordinary Recover is unchanged. */
  close_incident_recovery?: { expected_incident_id: string } | null;
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
  /**
   * CP9: present only when the operator's `recover` of a component without an
   * open incident cleared a latched stream condition (no transaction).
   */
  streamAcknowledgement?: { controllableComponentId: string; acknowledgedAtNs: Nanoseconds };
}

/** The operator's accepted acknowledgement of one open component incident. */
export interface CCLCIncidentRecovery {
  incidentId: string;
  controllableComponentId: string;
  causeClass: CCLCIncidentCauseClass;
  acknowledgedBy: string;
  acknowledgedAtNs: Nanoseconds;
  closureReason?: 'cell_closed_by_operator';
}

/** Bounded new owner commands preserve optional correlation presence exactly. */
export type BoundedPublicRequestContext = Omit<CCLCRequestContext, 'correlation_id'> & { correlation_id?: string | null };

/** Exact installed launch occurrence, never derived from a provider epoch alone. */
export interface ComponentProcessStopRequest {
  request_context: BoundedPublicRequestContext;
  predecessor_projection_revision: import('./api').ProjectionRevision;
  expected_predecessor_publication_generation: number;
  expected_predecessor_public_pointer_digest: string;
  runtime_unit_id: string;
  runtime_epoch: string;
  manager_runtime_epoch: string;
  controllable_component_id: string;
  launch_identity: string;
  leader_pid: number;
}
export interface ComponentProcessStopResult {
  request: ComponentProcessStopRequest;
  state: 'not_requested' | 'pending' | 'unknown' | 'drained';
  receipt: {
    leader_pid: number;
    controllable_component_id: string;
    launch_identity: string;
    observed_utc: string;
  } | null;
  error: CCLCTransactionStatus['error'];
}

export interface ControllerLaunchProcessOccurrence {
  process_owner_kind?: never;
  controllable_component_id: string; launch_identity: string; leader_pid: number;
  process_presence: 'unknown' | 'absent' | 'present';
  cleanup_state: 'not_requested' | 'pending' | 'unknown' | 'drained';
  receipt: ComponentProcessStopResult['receipt']; installed_runtime_identity: string | null;
}

/** Manager verifies the immutable configuration and actual installed owner. */
export interface PneumaticNoneLocalProcessOccurrence {
  process_owner_kind: 'pneumatic_none_local';
  controllable_component_id: string;
  pneumatic_none_owner: {
    command_target_id: string;
    control_binding_id: string;
    control_binding_revision: string;
  };
  installed_runtime_identity: string;
  process_presence: 'absent';
}

export interface ComponentProcessStopOffer {
  controllable_component_id: string;
  process_inventories: Array<{
    request: Pick<ComponentProcessStopRequest, 'predecessor_projection_revision'
      | 'expected_predecessor_publication_generation' | 'expected_predecessor_public_pointer_digest'
      | 'runtime_unit_id' | 'runtime_epoch' | 'manager_runtime_epoch'>;
    process_occurrences: Array<ControllerLaunchProcessOccurrence | PneumaticNoneLocalProcessOccurrence>;
    error: null;
  }>;
}
