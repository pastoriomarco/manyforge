import { parseNanoseconds } from '../types/nanoseconds';
import type {
  CCLCAdmission,
  CCLCBlockingCauseClass,
  CCLCComponentExpectation,
  CCLCComponentSnapshot,
  CCLCOwnerIdentity,
  CCLCDesiredBindingRoute,
  CCLCComponentIncident,
  CCLCDesiredConfiguration,
  CCLCExpectedActiveConfiguration,
  CCLCIncidentCauseClass,
  CCLCIncidentRecovery,
  CCLCLifecycleIntent,
  CCLCLifecycleProgress,
  CCLCLifecycleProgressPhase,
  CCLCLifecycleState,
  CCLCLifecycleTarget,
  CCLCLifecycleTargetOrigin,
  CCLCLifecycleTargetState,
  CCLCRouteTemplate,
  CCLCRouteTemplateSet,
  CCLCTransactionStatus,
} from '../types/cclc';

type JsonObject = Record<string, unknown>;

const LIFECYCLE_STATES = new Set<CCLCLifecycleState>([
  'undeclared', 'declared', 'prepared', 'active', 'degraded', 'faulted',
  'disabling', 'disabled',
]);
const REPLAY_STATES = new Set([
  'accepted_new', 'replayed_exact', 'request_result_expired',
  'request_id_conflict', 'request_ledger_full', 'stale_request_owner_epoch',
]);
const TERMINAL_STATES = new Set([
  'succeeded', 'failed_known_no_effect', 'failed_partial_effect',
]);
const LIFECYCLE_PHASES = new Set([
  'accepted', 'acquiring_domains', 'containing', 'executing', 'verifying',
  'outcome_unknown', 'reconciling', 'succeeded', 'failed',
]);
const ROUTE_PHASES = new Set([
  'accepted', 'acquiring_fence', 'observing', 'deactivating', 'activating',
  'verifying', 'publishing', 'releasing_fence', 'outcome_unknown',
  'reconciling', 'succeeded', 'failed',
]);
const EFFECTS = new Set([
  'none', 'known_no_effect', 'effect_observed', 'partial_effect_observed',
  'effect_uncertain',
]);
const LIFECYCLE_ACTION_STATES = new Set([
  'reserved', 'not_started', 'executing', 'verifying', 'reconciling',
  'succeeded', 'failed', 'outcome_unknown',
]);
const TARGET_STATES = new Set<CCLCLifecycleTargetState>(['active', 'prepared', 'disabled']);
const TARGET_ORIGINS = new Set<CCLCLifecycleTargetOrigin>(['startup', 'operator', 'program']);
const PROGRESS_PHASES = new Set<CCLCLifecycleProgressPhase>([
  'satisfied', 'waiting_for_prerequisites', 'transition_in_progress',
  'outcome_uncertain', 'blocked', 'cancelled',
]);
const LIFECYCLE_INTENTS = new Set<CCLCLifecycleIntent>([
  'prepare', 'enable', 'disable', 'recover',
]);
const BLOCKING_CAUSE_CLASSES = new Set<CCLCBlockingCauseClass>([
  'none', 'availability', 'emergency_stop', 'protective_stop', 'safeguard_stop',
  'fault', 'configuration', 'unknown', 'collision', 'external_control',
  'provider_lost',
]);
const INCIDENT_CAUSE_CLASSES = new Set<CCLCIncidentCauseClass>([
  'emergency_stop', 'protective_stop', 'safeguard_stop', 'fault',
  'configuration', 'unknown', 'collision', 'external_control', 'provider_lost',
]);
// Owner-issued diagnostic text; wider than identity text but still bounded.
const MAX_BLOCKING_REASON_BYTES = 1024;
const MAX_INCIDENT_ID_BYTES = 128;
const MAX_INCIDENT_SUMMARY_BYTES = 1024;
const MAX_VENDOR_CODES = 16;
const MAX_VENDOR_CODE_BYTES = 64;

function fail(path: string, expected: string): never {
  throw new Error(`${path} must be ${expected}`);
}

function objectAt(value: unknown, path: string): JsonObject {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) {
    return fail(path, 'an object');
  }
  return value as JsonObject;
}

function exact(value: JsonObject, keys: readonly string[], path: string): void {
  const actual = Object.keys(value).sort();
  const expected = [...keys].sort();
  if (
    actual.length !== expected.length
    || actual.some((key, index) => key !== expected[index])
  ) {
    fail(path, `the closed fields [${expected.join(', ')}]`);
  }
}

function textAt(value: unknown, path: string): string {
  if (
    typeof value !== 'string'
    || value.length === 0
    || value.normalize('NFC') !== value
    || new TextEncoder().encode(value).length > 256
  ) {
    return fail(path, 'bounded non-empty NFC text');
  }
  return value;
}

function integerAt(value: unknown, path: string): number {
  if (!Number.isSafeInteger(value) || (value as number) < 1) {
    return fail(path, 'a positive safe integer');
  }
  return value as number;
}

function safeIntegerAt(value: unknown, path: string): number {
  if (!Number.isSafeInteger(value)) return fail(path, 'a safe integer');
  return value as number;
}

function booleanAt(value: unknown, path: string): boolean {
  if (typeof value !== 'boolean') return fail(path, 'boolean');
  return value;
}

function arrayAt(value: unknown, path: string, maximum = 4096): unknown[] {
  if (!Array.isArray(value) || value.length > maximum) {
    return fail(path, `an array of at most ${maximum} rows`);
  }
  return value;
}

function canonicalBy<T>(
  rows: T[],
  key: (row: T) => string,
  path: string,
  { nonempty = false }: { nonempty?: boolean } = {},
): T[] {
  const keys = rows.map(key);
  if (
    (nonempty && rows.length === 0)
    || keys.some((value, index) => index > 0 && value <= keys[index - 1]!)
  ) {
    fail(path, 'a canonical strictly ordered collection');
  }
  return rows;
}

function nullableTextAt(value: unknown, path: string): string | null {
  return value === null ? null : textAt(value, path);
}

function errorAt(value: unknown, path: string): CCLCTransactionStatus['error'] {
  if (value === null) return null;
  const row = objectAt(value, path);
  exact(row, ['code', 'details', 'message'], path);
  const code = textAt(row.code, `${path}.code`);
  const message = textAt(row.message, `${path}.message`);
  const details = objectAt(row.details, `${path}.details`);
  if (Object.keys(details).length > 64) fail(`${path}.details`, 'at most 64 rows');
  const parsedDetails: Record<string, string> = {};
  Object.entries(details).forEach(([key, detail]) => {
    textAt(key, `${path}.details key`);
    parsedDetails[key] = textAt(detail, `${path}.details.${key}`);
  });
  return { code, message, details: parsedDetails };
}

export function parseCCLCOwnerIdentity(value: unknown): CCLCOwnerIdentity {
  const row = objectAt(value, 'cclcOwner');
  exact(row, ['request_owner_epoch'], 'cclcOwner');
  return {
    requestOwnerEpoch: textAt(
      row.request_owner_epoch,
      'cclcOwner.request_owner_epoch',
    ),
  };
}

function canonicalTextArrayAt(
  value: unknown,
  path: string,
  maximum = 4096,
): string[] {
  const rows = arrayAt(value, path, maximum).map((row, index) => (
    textAt(row, `${path}[${index}]`)
  ));
  return canonicalBy(rows, (row) => row, path);
}

function bindingAt(value: unknown, path: string): CCLCDesiredBindingRoute {
  const row = objectAt(value, path);
  exact(row, [
    'control_binding_id', 'control_binding_revision', 'control_route_id',
  ], path);
  return {
    control_binding_id: textAt(row.control_binding_id, `${path}.control_binding_id`),
    control_binding_revision: textAt(
      row.control_binding_revision,
      `${path}.control_binding_revision`,
    ),
    control_route_id: textAt(row.control_route_id, `${path}.control_route_id`),
  };
}

function expectedActiveAt(
  value: unknown,
  path: string,
): CCLCExpectedActiveConfiguration {
  const row = objectAt(value, path);
  const presence = textAt(row.presence, `${path}.presence`);
  if (presence !== 'absent' && presence !== 'present') {
    fail(`${path}.presence`, 'absent or present');
  }
  exact(
    row,
    presence === 'present'
      ? [
        'presence', 'active_control_configuration_id',
        'active_control_configuration_generation',
      ]
      : ['presence'],
    path,
  );
  const id = row.active_control_configuration_id === undefined
    || row.active_control_configuration_id === null
    ? null
    : textAt(
      row.active_control_configuration_id,
      `${path}.active_control_configuration_id`,
    );
  const generation = row.active_control_configuration_generation === undefined
    || row.active_control_configuration_generation === null
    ? null
    : integerAt(
      row.active_control_configuration_generation,
      `${path}.active_control_configuration_generation`,
    );
  if ((presence === 'present') !== (id !== null && generation !== null)) {
    fail(path, 'a consistent active-configuration presence union');
  }
  return {
    presence,
    active_control_configuration_id: id,
    active_control_configuration_generation: generation,
  };
}

function desiredAt(value: unknown, path: string): CCLCDesiredConfiguration {
  const row = objectAt(value, path);
  exact(row, [
    'activation_set_id', 'binding_routes', 'control_domain_id',
    'control_domain_revision', 'controller_topology_id', 'topology_revision',
  ], path);
  const routes = canonicalBy(
    arrayAt(row.binding_routes, `${path}.binding_routes`).map(
      (candidate, index) => bindingAt(candidate, `${path}.binding_routes[${index}]`),
    ),
    (candidate) => candidate.control_binding_id,
    `${path}.binding_routes`,
    { nonempty: true },
  );
  return {
    activation_set_id: textAt(row.activation_set_id, `${path}.activation_set_id`),
    binding_routes: routes,
    control_domain_id: textAt(row.control_domain_id, `${path}.control_domain_id`),
    control_domain_revision: textAt(
      row.control_domain_revision,
      `${path}.control_domain_revision`,
    ),
    controller_topology_id: textAt(
      row.controller_topology_id,
      `${path}.controller_topology_id`,
    ),
    topology_revision: textAt(row.topology_revision, `${path}.topology_revision`),
  };
}

function expectationAt(value: unknown, path: string): CCLCComponentExpectation {
  const row = objectAt(value, path);
  exact(row, [
    'controllable_component_id', 'expected_component_lifecycle_generation',
    'expected_component_lifecycle_state',
  ], path);
  const state = textAt(
    row.expected_component_lifecycle_state,
    `${path}.expected_component_lifecycle_state`,
  );
  if (state !== 'prepared' && state !== 'active' && state !== 'disabled') {
    fail(`${path}.expected_component_lifecycle_state`, 'prepared, active or disabled');
  }
  return {
    controllable_component_id: textAt(
      row.controllable_component_id,
      `${path}.controllable_component_id`,
    ),
    expected_component_lifecycle_generation: integerAt(
      row.expected_component_lifecycle_generation,
      `${path}.expected_component_lifecycle_generation`,
    ),
    expected_component_lifecycle_state: state,
  };
}

function templateAt(value: unknown, path: string): CCLCRouteTemplate {
  const row = objectAt(value, path);
  exact(row, [
    'component_expectations', 'desired_configuration',
    'expected_active_control_configuration',
  ], path);
  return {
    desired_configuration: desiredAt(
      row.desired_configuration,
      `${path}.desired_configuration`,
    ),
    expected_active_control_configuration: expectedActiveAt(
      row.expected_active_control_configuration,
      `${path}.expected_active_control_configuration`,
    ),
    component_expectations: canonicalBy(
      arrayAt(row.component_expectations, `${path}.component_expectations`).map(
        (candidate, index) => expectationAt(
          candidate,
          `${path}.component_expectations[${index}]`,
        ),
      ),
      (candidate) => candidate.controllable_component_id,
      `${path}.component_expectations`,
      { nonempty: true },
    ),
  };
}

function closedTextAt<T extends string>(
  value: unknown,
  allowed: ReadonlySet<T>,
  path: string,
  expected: string,
): T {
  const spelling = textAt(value, path);
  if (!allowed.has(spelling as T)) fail(path, expected);
  return spelling as T;
}

function lifecycleTargetAt(value: unknown, path: string): CCLCLifecycleTarget | null {
  if (value === null) return null;
  const row = objectAt(value, path);
  exact(row, ['origin', 'requested_state', 'target_generation'], path);
  return {
    requestedState: closedTextAt(
      row.requested_state, TARGET_STATES, `${path}.requested_state`,
      'active, prepared or disabled',
    ),
    origin: closedTextAt(
      row.origin, TARGET_ORIGINS, `${path}.origin`, 'startup, operator or program',
    ),
    targetGeneration: integerAt(row.target_generation, `${path}.target_generation`),
  };
}

function boundedNfcText(value: unknown, maximumBytes: number): value is string {
  return typeof value === 'string'
    && value.length > 0
    && value.normalize('NFC') === value
    && new TextEncoder().encode(value).length <= maximumBytes;
}

function incidentAt(value: unknown, path: string): CCLCComponentIncident | null {
  if (value === null) return null;
  const row = objectAt(value, path);
  exact(row, [
    'acknowledged', 'cause_class', 'detected_at_ns', 'incident_id', 'summary',
    'vendor_codes',
  ], path);
  if (!boundedNfcText(row.incident_id, MAX_INCIDENT_ID_BYTES)) {
    fail(`${path}.incident_id`, 'bounded non-empty NFC text');
  }
  if (!boundedNfcText(row.summary, MAX_INCIDENT_SUMMARY_BYTES)) {
    fail(`${path}.summary`, 'bounded non-empty NFC text');
  }
  const codes = row.vendor_codes;
  if (
    !Array.isArray(codes)
    || codes.length > MAX_VENDOR_CODES
    || !codes.every((code) => boundedNfcText(code, MAX_VENDOR_CODE_BYTES))
  ) {
    fail(`${path}.vendor_codes`, `at most ${MAX_VENDOR_CODES} bounded vendor codes`);
  }
  if (row.acknowledged !== false) fail(`${path}.acknowledged`, 'false');
  return {
    incidentId: row.incident_id,
    causeClass: closedTextAt(
      row.cause_class, INCIDENT_CAUSE_CLASSES, `${path}.cause_class`,
      'an intervention cause class',
    ),
    vendorCodes: [...codes] as string[],
    detectedAtNs: parseNanoseconds(row.detected_at_ns, `${path}.detected_at_ns`),
    summary: row.summary,
    acknowledged: false,
  };
}

function lifecycleProgressAt(
  value: unknown,
  path: string,
): CCLCLifecycleProgress | null {
  if (value === null) return null;
  const row = objectAt(value, path);
  exact(row, [
    'blocking_cause_class', 'blocking_reason', 'delayed', 'incident',
    'next_intent', 'phase', 'waiting_since_ns',
  ], path);
  const reason = row.blocking_reason;
  if (reason !== null && !boundedNfcText(reason, MAX_BLOCKING_REASON_BYTES)) {
    fail(`${path}.blocking_reason`, 'bounded non-empty NFC text or null');
  }
  return {
    phase: closedTextAt(
      row.phase, PROGRESS_PHASES, `${path}.phase`, 'a known lifecycle progress phase',
    ),
    nextIntent: row.next_intent === null
      ? null
      : closedTextAt(
        row.next_intent, LIFECYCLE_INTENTS, `${path}.next_intent`,
        'a known lifecycle intent or null',
      ),
    blockingCauseClass: closedTextAt(
      row.blocking_cause_class, BLOCKING_CAUSE_CLASSES,
      `${path}.blocking_cause_class`, 'a known blocking cause class',
    ),
    blockingReason: reason as string | null,
    waitingSinceNs: row.waiting_since_ns === null
      ? null
      : parseNanoseconds(row.waiting_since_ns, `${path}.waiting_since_ns`),
    delayed: booleanAt(row.delayed, `${path}.delayed`),
    incident: incidentAt(row.incident, `${path}.incident`),
  };
}

export function parseCCLCComponentSnapshot(value: unknown): CCLCComponentSnapshot {
  const row = objectAt(value, 'cclcComponent');
  exact(row, [
    'active_public_pointer_digest', 'component_lifecycle_generation',
    'component_lifecycle_state', 'controllable_component_id', 'domains',
    'graph_revision', 'lifecycle_progress', 'lifecycle_target',
    'provider_occurrences', 'publication_generation',
    'retained_bundle_id', 'snapshot_digest',
  ], 'cclcComponent');
  const state = textAt(
    row.component_lifecycle_state,
    'cclcComponent.component_lifecycle_state',
  ) as CCLCLifecycleState;
  if (!LIFECYCLE_STATES.has(state)) {
    fail('cclcComponent.component_lifecycle_state', 'a known lifecycle state');
  }
  const domains = canonicalBy(
    arrayAt(row.domains, 'cclcComponent.domains', 256).map((candidate, index) => {
      const path = `cclcComponent.domains[${index}]`;
      const domain = objectAt(candidate, path);
      exact(domain, [
        'active_control_configuration', 'control_domain_id',
        'control_domain_revision', 'controller_topology_id',
        'current_control_bindings', 'current_resources', 'topology_revision',
      ], path);
      let activeConfiguration = null;
      if (domain.active_control_configuration !== null) {
        const active = objectAt(
          domain.active_control_configuration,
          `${path}.active_control_configuration`,
        );
        exact(active, [
          'activation_set_id', 'active_control_configuration_generation',
          'active_control_configuration_id', 'binding_routes',
          'control_domain_id', 'control_domain_revision',
          'controller_topology_id', 'source_runtime_epoch',
          'source_runtime_unit_id', 'source_sequence', 'state',
          'topology_revision',
        ], `${path}.active_control_configuration`);
        const id = textAt(
          active.active_control_configuration_id,
          `${path}.active_control_configuration.active_control_configuration_id`,
        );
        const generation = integerAt(
          active.active_control_configuration_generation,
          `${path}.active_control_configuration.active_control_configuration_generation`,
        );
        textAt(active.activation_set_id, `${path}.active_control_configuration.activation_set_id`);
        desiredAt({
          activation_set_id: active.activation_set_id,
          binding_routes: active.binding_routes,
          control_domain_id: active.control_domain_id,
          control_domain_revision: active.control_domain_revision,
          controller_topology_id: active.controller_topology_id,
          topology_revision: active.topology_revision,
        }, `${path}.active_control_configuration`);
        textAt(active.source_runtime_epoch, `${path}.active_control_configuration.source_runtime_epoch`);
        textAt(active.source_runtime_unit_id, `${path}.active_control_configuration.source_runtime_unit_id`);
        integerAt(active.source_sequence, `${path}.active_control_configuration.source_sequence`);
        const stateValue = textAt(active.state, `${path}.active_control_configuration.state`);
        if (stateValue !== 'reconciled' && stateValue !== 'external_or_unknown_unreconciled') {
          fail(`${path}.active_control_configuration.state`, 'a known configuration state');
        }
        activeConfiguration = { id, generation };
      }
      const bindings = canonicalBy(
        arrayAt(
          domain.current_control_bindings,
          `${path}.current_control_bindings`,
        ).map((binding, bindingIndex) => {
          const parsed = bindingAt(
            binding,
            `${path}.current_control_bindings[${bindingIndex}]`,
          );
          return {
            controlBindingId: parsed.control_binding_id,
            controlBindingRevision: parsed.control_binding_revision,
            controlRouteId: parsed.control_route_id,
          };
        }),
        (binding) => binding.controlBindingId,
        `${path}.current_control_bindings`,
        { nonempty: true },
      );
      const resources = canonicalTextArrayAt(
        domain.current_resources,
        `${path}.current_resources`,
      );
      if (resources.length === 0) fail(`${path}.current_resources`, 'a non-empty array');
      return {
        controlDomainId: textAt(domain.control_domain_id, `${path}.control_domain_id`),
        controlDomainRevision: textAt(
          domain.control_domain_revision,
          `${path}.control_domain_revision`,
        ),
        controllerTopologyId: textAt(
          domain.controller_topology_id,
          `${path}.controller_topology_id`,
        ),
        topologyRevision: textAt(domain.topology_revision, `${path}.topology_revision`),
        activeConfiguration,
        currentControlBindings: bindings,
        currentResources: resources,
      };
    }),
    (domain) => domain.controlDomainId,
    'cclcComponent.domains',
    { nonempty: true },
  );
  const providers = arrayAt(
    row.provider_occurrences,
    'cclcComponent.provider_occurrences',
    4096,
  ).map((provider, index) => {
    const path = `cclcComponent.provider_occurrences[${index}]`;
    const candidate = objectAt(provider, path);
    exact(candidate, [
      'control_domain_id', 'lifecycle_plan_catalog_digest',
      'provider_instance_id', 'provider_manifest_id',
      'provider_manifest_revision', 'provider_runtime_epoch',
      'provider_runtime_unit_id',
    ], path);
    Object.entries(candidate).forEach(([key, value]) => textAt(value, `${path}.${key}`));
    return {
      controlDomainId: candidate.control_domain_id as string,
      lifecyclePlanCatalogDigest: candidate.lifecycle_plan_catalog_digest as string,
      providerInstanceId: candidate.provider_instance_id as string,
      providerManifestId: candidate.provider_manifest_id as string,
      providerManifestRevision: candidate.provider_manifest_revision as string,
      providerRuntimeEpoch: candidate.provider_runtime_epoch as string,
      providerRuntimeUnitId: candidate.provider_runtime_unit_id as string,
    };
  });
  canonicalBy(
    providers,
    (provider) => `${provider.controlDomainId}\u0000${provider.providerInstanceId}`,
    'cclcComponent.provider_occurrences',
  );
  integerAt(row.publication_generation, 'cclcComponent.publication_generation');
  textAt(row.snapshot_digest, 'cclcComponent.snapshot_digest');
  textAt(row.active_public_pointer_digest, 'cclcComponent.active_public_pointer_digest');
  return {
    componentId: textAt(
      row.controllable_component_id,
      'cclcComponent.controllable_component_id',
    ),
    lifecycleGeneration: integerAt(
      row.component_lifecycle_generation,
      'cclcComponent.component_lifecycle_generation',
    ),
    lifecycleState: state,
    retainedBundleId: textAt(row.retained_bundle_id, 'cclcComponent.retained_bundle_id'),
    graphRevision: textAt(row.graph_revision, 'cclcComponent.graph_revision'),
    domains,
    providers,
    lifecycleTarget: lifecycleTargetAt(
      row.lifecycle_target,
      'cclcComponent.lifecycle_target',
    ),
    lifecycleProgress: lifecycleProgressAt(
      row.lifecycle_progress,
      'cclcComponent.lifecycle_progress',
    ),
  };
}

export function parseCCLCRouteTemplateSet(value: unknown): CCLCRouteTemplateSet {
  const row = objectAt(value, 'cclcRouteTemplates');
  exact(row, [
    'controllable_component_id', 'graph_revision', 'retained_bundle_id',
    'templates',
  ], 'cclcRouteTemplates');
  const templates = canonicalBy(
    arrayAt(row.templates, 'cclcRouteTemplates.templates', 4096).map(
      (candidate, index) => templateAt(
        candidate,
        `cclcRouteTemplates.templates[${index}]`,
      ),
    ),
    (template) => template.desired_configuration.control_domain_id,
    'cclcRouteTemplates.templates',
  );
  return {
    componentId: textAt(
      row.controllable_component_id,
      'cclcRouteTemplates.controllable_component_id',
    ),
    graphRevision: textAt(row.graph_revision, 'cclcRouteTemplates.graph_revision'),
    retainedBundleId: textAt(
      row.retained_bundle_id,
      'cclcRouteTemplates.retained_bundle_id',
    ),
    templates,
  };
}

function statusAt(
  value: unknown,
  kind: 'lifecycle' | 'route',
  path: string,
): CCLCTransactionStatus {
  const row = objectAt(value, path);
  const transactionField = kind === 'lifecycle'
    ? 'lifecycle_transaction_id'
    : 'route_selection_transaction_id';
  exact(row, kind === 'lifecycle' ? [
    'acceptance', 'accepted_at_ns', 'controllable_component_id',
    'current_state', 'domains', 'error', 'latest_updated_at_ns', 'phase',
    'previous_state', 'reconciliation_required', 'remaining_duration_ns',
    'status_owner_epoch', 'status_sequence', 'terminal_result',
  ] : [
    'acceptance', 'accepted_at_ns', 'affected_control_binding_ids',
    'affected_controller_role_ids', 'effect', 'error', 'latest_updated_at_ns',
    'phase', 'reconciliation_required', 'remaining_duration_ns',
    'resulting_active_control_configuration', 'status_owner_epoch',
    'status_sequence', 'terminal_result',
  ], path);
  const acceptance = objectAt(row.acceptance, `${path}.acceptance`);
  exact(acceptance, kind === 'lifecycle' ? [
    'accepted_at_ns', 'component_lifecycle_generation',
    'lifecycle_transaction_generation', 'lifecycle_transaction_id',
    'request_id', 'timeout_duration_ns',
  ] : [
    'accepted_at_ns', 'candidate_active_control_configuration_generation',
    'candidate_active_control_configuration_id', 'control_domain_id',
    'request_id', 'route_selection_transaction_generation',
    'route_selection_transaction_id', 'timeout_duration_ns',
  ], `${path}.acceptance`);
  parseNanoseconds(acceptance.accepted_at_ns, `${path}.acceptance.accepted_at_ns`);
  safeIntegerAt(acceptance.timeout_duration_ns, `${path}.acceptance.timeout_duration_ns`);
  textAt(acceptance.request_id, `${path}.acceptance.request_id`);
  if (kind === 'lifecycle') {
    integerAt(acceptance.component_lifecycle_generation, `${path}.acceptance.component_lifecycle_generation`);
    integerAt(acceptance.lifecycle_transaction_generation, `${path}.acceptance.lifecycle_transaction_generation`);
    textAt(row.controllable_component_id, `${path}.controllable_component_id`);
    for (const [field, state] of [
      ['current_state', row.current_state],
      ['previous_state', row.previous_state],
    ] as const) {
      const spelling = nullableTextAt(state, `${path}.${field}`);
      if (spelling !== null && !LIFECYCLE_STATES.has(spelling as CCLCLifecycleState)) {
        fail(`${path}.${field}`, 'a known lifecycle state or null');
      }
    }
    arrayAt(row.domains, `${path}.domains`, 256).forEach((candidate, index) => {
      const domainPath = `${path}.domains[${index}]`;
      const domain = objectAt(candidate, domainPath);
      exact(domain, ['action_state', 'control_domain_id', 'current_state', 'effect', 'error'], domainPath);
      if (!LIFECYCLE_ACTION_STATES.has(textAt(domain.action_state, `${domainPath}.action_state`))) {
        fail(`${domainPath}.action_state`, 'a known lifecycle action state');
      }
      textAt(domain.control_domain_id, `${domainPath}.control_domain_id`);
      const current = nullableTextAt(domain.current_state, `${domainPath}.current_state`);
      if (current !== null && !LIFECYCLE_STATES.has(current as CCLCLifecycleState)) {
        fail(`${domainPath}.current_state`, 'a known lifecycle state or null');
      }
      if (!EFFECTS.has(textAt(domain.effect, `${domainPath}.effect`))) {
        fail(`${domainPath}.effect`, 'a known lifecycle effect');
      }
      errorAt(domain.error, `${domainPath}.error`);
    });
  } else {
    integerAt(
      acceptance.candidate_active_control_configuration_generation,
      `${path}.acceptance.candidate_active_control_configuration_generation`,
    );
    textAt(
      acceptance.candidate_active_control_configuration_id,
      `${path}.acceptance.candidate_active_control_configuration_id`,
    );
    textAt(acceptance.control_domain_id, `${path}.acceptance.control_domain_id`);
    canonicalTextArrayAt(row.affected_control_binding_ids, `${path}.affected_control_binding_ids`);
    canonicalTextArrayAt(row.affected_controller_role_ids, `${path}.affected_controller_role_ids`);
    if (!EFFECTS.has(textAt(row.effect, `${path}.effect`))) {
      fail(`${path}.effect`, 'a known route effect');
    }
    if (row.resulting_active_control_configuration !== null) {
      const active = objectAt(
        row.resulting_active_control_configuration,
        `${path}.resulting_active_control_configuration`,
      );
      exact(active, [
        'activation_set_id', 'active_control_configuration_generation',
        'active_control_configuration_id', 'binding_routes',
        'control_domain_id', 'control_domain_revision', 'controller_topology_id',
        'source_runtime_epoch', 'source_runtime_unit_id', 'source_sequence',
        'state', 'topology_revision',
      ], `${path}.resulting_active_control_configuration`);
    }
  }
  parseNanoseconds(row.accepted_at_ns, `${path}.accepted_at_ns`);
  parseNanoseconds(row.latest_updated_at_ns, `${path}.latest_updated_at_ns`);
  safeIntegerAt(row.remaining_duration_ns, `${path}.remaining_duration_ns`);
  textAt(row.status_owner_epoch, `${path}.status_owner_epoch`);
  integerAt(row.status_sequence, `${path}.status_sequence`);
  const error = errorAt(row.error, `${path}.error`);
  const phase = textAt(row.phase, `${path}.phase`);
  if (!(kind === 'lifecycle' ? LIFECYCLE_PHASES : ROUTE_PHASES).has(phase)) {
    fail(`${path}.phase`, 'a known transaction phase');
  }
  const terminalRaw = row.terminal_result;
  let terminalResult: CCLCTransactionStatus['terminalResult'] = null;
  if (terminalRaw !== null) {
    const terminal = textAt(terminalRaw, `${path}.terminal_result`);
    if (!TERMINAL_STATES.has(terminal)) {
      fail(`${path}.terminal_result`, 'a known terminal result or null');
    }
    terminalResult = terminal as Exclude<
      CCLCTransactionStatus['terminalResult'], null
    >;
  }
  return {
    kind,
    transactionId: textAt(
      acceptance[transactionField],
      `${path}.acceptance.${transactionField}`,
    ),
    phase,
    terminalResult,
    error,
    reconciliationRequired: booleanAt(
      row.reconciliation_required,
      `${path}.reconciliation_required`,
    ),
  };
}

export function parseCCLCAdmission(
  value: unknown,
  kind: 'lifecycle' | 'route',
): CCLCAdmission {
  const row = objectAt(value, 'cclcAdmission');
  const recovered = kind === 'lifecycle' && 'incident_recovery' in row;
  exact(
    row,
    recovered
      ? ['acceptance', 'incident_recovery', 'replay_status', 'status']
      : ['acceptance', 'replay_status', 'status'],
    'cclcAdmission',
  );
  const replayStatus = textAt(row.replay_status, 'cclcAdmission.replay_status');
  if (!REPLAY_STATES.has(replayStatus)) {
    fail('cclcAdmission.replay_status', 'a known replay disposition');
  }
  let requestId: string | null = null;
  let transactionId: string | null = null;
  if (row.acceptance !== null) {
    const acceptance = objectAt(row.acceptance, 'cclcAdmission.acceptance');
    exact(acceptance, kind === 'lifecycle' ? [
      'accepted_at_ns', 'component_lifecycle_generation',
      'lifecycle_transaction_generation', 'lifecycle_transaction_id',
      'request_id', 'timeout_duration_ns',
    ] : [
      'accepted_at_ns', 'candidate_active_control_configuration_generation',
      'candidate_active_control_configuration_id', 'control_domain_id',
      'request_id', 'route_selection_transaction_generation',
      'route_selection_transaction_id', 'timeout_duration_ns',
    ], 'cclcAdmission.acceptance');
    requestId = textAt(acceptance.request_id, 'cclcAdmission.acceptance.request_id');
    transactionId = textAt(
      acceptance[
        kind === 'lifecycle'
          ? 'lifecycle_transaction_id'
          : 'route_selection_transaction_id'
      ],
      'cclcAdmission.acceptance.transaction_id',
    );
  }
  const status = row.status === null
    ? null
    : statusAt(row.status, kind, 'cclcAdmission.status');
  if (status !== null && status.transactionId !== transactionId) {
    fail('cclcAdmission.status', 'the accepted transaction status');
  }
  let incidentRecovery: CCLCIncidentRecovery | null = null;
  if (recovered) {
    // Authorization-only: no transaction, and only an accepted or replayed
    // request can carry it.
    if (
      row.acceptance !== null
      || row.status !== null
      || (replayStatus !== 'accepted_new' && replayStatus !== 'replayed_exact')
    ) {
      fail('cclcAdmission.incident_recovery', 'an authorization-only accepted Recover');
    }
    incidentRecovery = incidentRecoveryAt(row.incident_recovery, 'cclcAdmission.incident_recovery');
  }
  return {
    replayStatus: replayStatus as CCLCAdmission['replayStatus'],
    requestId,
    transactionId,
    status,
    incidentRecovery,
  };
}

function incidentRecoveryAt(value: unknown, path: string): CCLCIncidentRecovery {
  const row = objectAt(value, path);
  exact(row, [
    'acknowledged_at_ns', 'acknowledged_by', 'cause_class', 'controllable_component_id',
    'incident_id',
  ], path);
  if (!boundedNfcText(row.incident_id, MAX_INCIDENT_ID_BYTES)) {
    fail(`${path}.incident_id`, 'bounded non-empty NFC text');
  }
  return {
    incidentId: row.incident_id,
    componentId: textAt(row.controllable_component_id, `${path}.controllable_component_id`),
    causeClass: closedTextAt(
      row.cause_class, INCIDENT_CAUSE_CLASSES, `${path}.cause_class`,
      'an intervention cause class',
    ),
    acknowledgedBy: textAt(row.acknowledged_by, `${path}.acknowledged_by`),
    acknowledgedAtNs: parseNanoseconds(row.acknowledged_at_ns, `${path}.acknowledged_at_ns`),
  };
}

export function parseCCLCStatusEnvelope(
  value: unknown,
  kind: 'lifecycle' | 'route',
): CCLCTransactionStatus | null {
  const row = objectAt(value, 'cclcStatusEnvelope');
  exact(row, ['status'], 'cclcStatusEnvelope');
  return row.status === null ? null : statusAt(row.status, kind, 'cclcStatusEnvelope.status');
}
