import type { ManagerApplyPhase } from '../../types/api';
import type {
  ActiveConfigurationIdentity,
  AlarmLifecycleState,
  AlarmMembership,
  AlarmMotionAttentionMember,
  AlarmPriority,
  AlarmSubject,
  ComponentOverviewProjection,
  ComposerHeadlessState,
  HeadlessRuntimeSnapshot,
  HeadlessSourceSection,
  KernelAlarmAttention,
  OperatorAlarmRecord,
  OperatorAlarmSnapshot,
} from '../../types/headless';
import type {
  CellRecoverResult,
  GlobalRuleRecoveryOffer,
  GlobalRuleRecoveryResult,
  GlobalRuleRecoverySelector,
  StopFaultRecoveryOffer,
} from '../../types/robotControl';
import {
  allowedKeysAt,
  arrayAt,
  booleanAt,
  boundedArrayAt,
  digestAt,
  exactKeysAt,
  fail,
  integerAt,
  nullablePositiveUint64TextAt,
  monotonicNanosecondsAt,
  nullableNumberAt,
  nullableStringAt,
  objectAt,
  positiveIntegerAt,
  sourceFreshnessAt,
  stringArrayAt,
  stringAt,
} from './shared';
import { MANAGER_APPLY_PHASES } from './manager';
import { parseAuthoringWorkspaceContainment, parseAuthoringWorkspaceIdentity } from './workspace';
import { parseProgramCycleStatus } from './programRuntime';
import { parseExecutionControlStatus } from './execution';

function parseHeadlessSourceSection(
  value: unknown,
  path: string,
): HeadlessSourceSection {
  const record = objectAt(value, path);
  return {
    available: booleanAt(record.available, `${path}.available`),
    freshness: sourceFreshnessAt(record.freshness, `${path}.freshness`),
    knownGaps: stringArrayAt(record.known_gaps, `${path}.known_gaps`),
    payload: record.payload === null
      ? null
      : objectAt(record.payload, `${path}.payload`),
    sourceGeneration: nullableNumberAt(
      record.source_generation,
      `${path}.source_generation`,
    ),
    sourceOwnerEpoch: nullableStringAt(
      record.source_owner_epoch,
      `${path}.source_owner_epoch`,
    ),
    sourceOwnerId: nullableStringAt(
      record.source_owner_id,
      `${path}.source_owner_id`,
    ),
    receiverAgeNs: nullableNumberAt(
      record.receiver_age_ns,
      `${path}.receiver_age_ns`,
    ),
  };
}

export function parseHeadlessRuntimeSnapshot(
  value: unknown,
): HeadlessRuntimeSnapshot {
  const record = objectAt(value, 'runtime');
  const schemaVersion = stringAt(record.schema_version, 'runtime.schema_version');
  if (schemaVersion !== 'manyforge.headless_runtime_snapshot.v1') {
    return fail(
      'runtime.schema_version',
      'manyforge.headless_runtime_snapshot.v1',
    );
  }
  const sections = objectAt(record.sections, 'runtime.sections');
  const reason = (value: unknown, path: string) => {
    const row = objectAt(value, path);
    return { code: stringAt(row.code, `${path}.code`), message: stringAt(row.message, `${path}.message`) };
  };
  const failure = record.initial_scene_failure == null ? null : objectAt(record.initial_scene_failure, 'runtime.initial_scene_failure');
  const failureState = failure === null ? null : stringAt(failure.state, 'runtime.initial_scene_failure.state');
  if (failureState !== null && failureState !== 'failed' && failureState !== 'unknown') return fail('runtime.initial_scene_failure.state', 'failed or unknown');
  return {
    initialSceneFailure: failure === null ? null : { state: failureState as 'failed' | 'unknown', reason: reason(failure.reason, 'runtime.initial_scene_failure.reason') },
    schemaVersion,
    activeConfiguration: parseActiveConfigurationPair(
      record.active_retained_bundle_id,
      record.active_graph_revision,
      'runtime.active_retained_bundle_id',
    ),
    complete: booleanAt(record.complete, 'runtime.complete'),
    projectionGeneration: integerAt(
      record.projection_generation,
      'runtime.projection_generation',
    ),
    projectionOwnerEpoch: stringAt(
      record.projection_owner_epoch,
      'runtime.projection_owner_epoch',
    ),
    projectionOwnerId: stringAt(
      record.projection_owner_id,
      'runtime.projection_owner_id',
    ),
    reasons: stringArrayAt(record.reasons, 'runtime.reasons'),
    unavailableSections: stringArrayAt(
      record.unavailable_sections,
      'runtime.unavailable_sections',
    ),
    sections: {
      manager: parseHeadlessSourceSection(
        sections.manager,
        'runtime.sections.manager',
      ),
      logicalScene: parseHeadlessSourceSection(
        sections.logical_scene,
        'runtime.sections.logical_scene',
      ),
      sceneRuntime: parseHeadlessSourceSection(
        sections.scene_runtime,
        'runtime.sections.scene_runtime',
      ),
    },
  };
}

function parseActiveConfigurationPair(
  retainedBundleId: unknown,
  graphRevision: unknown,
  path: string,
): ActiveConfigurationIdentity | null {
  const bundle = retainedBundleId === undefined ? null : nullableStringAt(retainedBundleId, path);
  const graph = graphRevision === undefined ? null : nullableStringAt(graphRevision, path);
  if (bundle === null && graph === null) return null;
  if (bundle === null || graph === null) {
    return fail(path, 'both a retained bundle id and a graph revision, or neither');
  }
  return { retainedBundleId: bundle, graphRevision: graph };
}

function parseKernelAlarmAttention(value: unknown): KernelAlarmAttention | null {
  // Absent from a composition without the Kernel publisher: no held copy.
  if (value === undefined || value === null) return null;
  const record = objectAt(value, 'alarms.kernel_attention');
  const configuration = record.active_configuration;
  return {
    ownerEpoch: stringAt(record.owner_epoch, 'alarms.kernel_attention.owner_epoch'),
    generation: integerAt(record.generation, 'alarms.kernel_attention.generation'),
    reconciled: booleanAt(record.reconciled, 'alarms.kernel_attention.reconciled'),
    activeConfiguration: configuration === null
      ? null
      : kernelAttentionConfiguration(configuration),
  };
}

function kernelAttentionConfiguration(value: unknown): ActiveConfigurationIdentity {
  const path = 'alarms.kernel_attention.active_configuration';
  const record = objectAt(value, path);
  return {
    retainedBundleId: stringAt(record.retained_bundle_id, `${path}.retained_bundle_id`),
    graphRevision: stringAt(record.graph_revision, `${path}.graph_revision`),
  };
}

function alarmLifecycleStateAt(
  value: unknown,
  path: string,
): AlarmLifecycleState {
  const state = stringAt(value, path);
  if (![
    'active_unacknowledged',
    'active_acknowledged',
    'returned_unacknowledged',
    'returned_acknowledged',
  ].includes(state)) {
    return fail(path, 'a supported four-state alarm lifecycle value');
  }
  return state as AlarmLifecycleState;
}

function alarmPriorityAt(value: unknown, path: string): AlarmPriority {
  const priority = stringAt(value, path);
  if (!['critical', 'high', 'medium', 'low'].includes(priority)) {
    return fail(path, 'critical, high, medium, or low');
  }
  return priority as AlarmPriority;
}

function parseAlarmSubject(value: unknown, path: string): AlarmSubject {
  const record = objectAt(value, path);
  return {
    subjectKind: stringAt(record.subject_kind, `${path}.subject_kind`),
    subjectId: stringAt(record.subject_id, `${path}.subject_id`),
    resourceIds: stringArrayAt(record.resource_ids, `${path}.resource_ids`),
  };
}

function parseAlarmMembership(value: unknown, path: string): AlarmMembership {
  const record = objectAt(value, path);
  return {
    conditionId: stringAt(record.condition_id, `${path}.condition_id`),
    activationId: stringAt(record.activation_id, `${path}.activation_id`),
    recordRevision: integerAt(record.record_revision, `${path}.record_revision`),
  };
}

function parseAlarmMotionAttentionMember(
  value: unknown,
  path: string,
): AlarmMotionAttentionMember {
  const record = objectAt(value, path);
  return {
    ...parseAlarmMembership(record, path),
    lifecycleState: alarmLifecycleStateAt(
      record.lifecycle_state,
      `${path}.lifecycle_state`,
    ),
    definitionId: stringAt(record.definition_id, `${path}.definition_id`),
    subject: parseAlarmSubject(record.subject, `${path}.subject`),
  };
}

function parseOperatorAlarmRecord(
  value: unknown,
  path: string,
): OperatorAlarmRecord {
  const record = objectAt(value, path);
  return {
    ...parseAlarmMembership(record, path),
    activatedAtNs: monotonicNanosecondsAt(record.activated_at_ns, `${path}.activated_at_ns`),
    definitionId: stringAt(record.definition_id, `${path}.definition_id`),
    lifecycleState: alarmLifecycleStateAt(
      record.lifecycle_state,
      `${path}.lifecycle_state`,
    ),
    active: booleanAt(record.active, `${path}.active`),
    acknowledged: booleanAt(record.acknowledged, `${path}.acknowledged`),
    priority: alarmPriorityAt(record.priority, `${path}.priority`),
    alarmClass: stringAt(record.alarm_class, `${path}.alarm_class`),
    title: stringAt(record.title, `${path}.title`),
    message: stringAt(record.message, `${path}.message`),
    consequence: stringAt(record.consequence, `${path}.consequence`),
    operatorResponse: stringAt(
      record.operator_response,
      `${path}.operator_response`,
    ),
    occurrenceCount: integerAt(
      record.occurrence_count,
      `${path}.occurrence_count`,
    ),
    acknowledgementRequired: booleanAt(
      record.acknowledgement_required,
      `${path}.acknowledgement_required`,
    ),
    requiresMotionAttention: booleanAt(
      record.requires_motion_attention,
      `${path}.requires_motion_attention`,
    ),
    subject: parseAlarmSubject(record.subject, `${path}.subject`),
    details: objectAt(record.details, `${path}.details`),
  };
}

export function parseOperatorAlarmSnapshot(
  value: unknown,
): OperatorAlarmSnapshot {
  const record = objectAt(value, 'alarms');
  const schemaVersion = stringAt(record.schema_version, 'alarms.schema_version');
  if (schemaVersion !== 'manyforge.operator_alarm_snapshot.v2') {
    return fail(
      'alarms.schema_version',
      'manyforge.operator_alarm_snapshot.v2',
    );
  }
  const health = objectAt(record.health, 'alarms.health');
  return {
    schemaVersion,
    ownerId: stringAt(record.owner_id, 'alarms.owner_id'),
    ownerEpoch: stringAt(record.owner_epoch, 'alarms.owner_epoch'),
    generation: integerAt(record.generation, 'alarms.generation'),
    stateRevision: integerAt(record.state_revision, 'alarms.state_revision'),
    reconciled: booleanAt(record.reconciled, 'alarms.reconciled'),
    healthState: stringAt(health.state, 'alarms.health.state'),
    liveCount: integerAt(record.live_count, 'alarms.live_count'),
    unacknowledgedCount: integerAt(
      record.unacknowledged_count,
      'alarms.unacknowledged_count',
    ),
    activeCount: integerAt(record.active_count, 'alarms.active_count'),
    activeAcknowledgedCount: integerAt(
      record.active_acknowledged_count,
      'alarms.active_acknowledged_count',
    ),
    motionAttentionCount: integerAt(
      record.motion_attention_count,
      'alarms.motion_attention_count',
    ),
    motionAttentionMembership: boundedArrayAt(
      record.motion_attention_membership,
      'alarms.motion_attention_membership',
      256,
    ).map((item, index) => parseAlarmMotionAttentionMember(
      item,
      `alarms.motion_attention_membership[${index}]`,
    )),
    unacknowledgedMembership: arrayAt(
      record.unacknowledged_membership,
      'alarms.unacknowledged_membership',
    ).map((item, index) => parseAlarmMembership(
      item,
      `alarms.unacknowledged_membership[${index}]`,
    )),
    alarms: arrayAt(record.alarms, 'alarms.alarms').map((item, index) =>
      parseOperatorAlarmRecord(item, `alarms.alarms[${index}]`)),
    truncated: booleanAt(record.truncated, 'alarms.truncated'),
    kernelAttention: parseKernelAlarmAttention(record.kernel_attention),
  };
}

export function parseComponentOverviewProjection(
  value: unknown,
  path: string,
): ComponentOverviewProjection {
  const record = objectAt(value, path);
  exactKeysAt(record, [
    'schema_version', 'overview_revision', 'readiness', 'summary_label',
    'profile_label', 'authority_label', 'user_message', 'capabilities',
    'components', 'maintenance_operation', 'basis',
  ], path);
  const schemaVersion = stringAt(record.schema_version, `${path}.schema_version`);
  if (schemaVersion !== 'manyforge.component_overview.v2') {
    return fail(`${path}.schema_version`, 'manyforge.component_overview.v2');
  }
  const readiness = stringAt(record.readiness, `${path}.readiness`);
  if (!['ready', 'waiting', 'degraded', 'fault', 'offline'].includes(readiness)) {
    return fail(`${path}.readiness`, 'a supported Component Overview readiness');
  }
  const componentStatuses = new Set([
    'healthy', 'waiting', 'warning', 'fault', 'offline', 'not_applicable',
  ]);
  const components = arrayAt(record.components, `${path}.components`).map(
    (valueAtIndex, index) => {
      const componentPath = `${path}.components[${index}]`;
      const component = objectAt(valueAtIndex, componentPath);
      exactKeysAt(component, [
        'id', 'label', 'status', 'detail', 'required', 'required_for',
        'owner_id', 'owner_epoch', 'generation', 'reason_code', 'scope',
        'contributor_ids',
      ], componentPath);
      const status = stringAt(component.status, `${componentPath}.status`);
      if (!componentStatuses.has(status)) {
        return fail(`${componentPath}.status`, 'a supported component status');
      }
      return {
        id: stringAt(component.id, `${componentPath}.id`),
        label: stringAt(component.label, `${componentPath}.label`),
        status: status as ComponentOverviewProjection['components'][number]['status'],
        detail: stringAt(component.detail, `${componentPath}.detail`),
        required: booleanAt(component.required, `${componentPath}.required`),
        requiredFor: stringArrayAt(component.required_for, `${componentPath}.required_for`),
        ownerId: nullableStringAt(component.owner_id, `${componentPath}.owner_id`),
        ownerEpoch: nullableStringAt(component.owner_epoch, `${componentPath}.owner_epoch`),
        generation: component.generation === null
          ? null : integerAt(component.generation, `${componentPath}.generation`),
        reasonCode: nullableStringAt(component.reason_code, `${componentPath}.reason_code`),
        scope: stringAt(component.scope, `${componentPath}.scope`),
        contributorIds: stringArrayAt(
          component.contributor_ids,
          `${componentPath}.contributor_ids`,
        ),
      };
    },
  );
  const basisPath = `${path}.basis`;
  const basis = objectAt(record.basis, basisPath);
  const basisKeys = [
    'runtime_projection_owner_epoch', 'runtime_projection_generation',
    'manager_source_owner_epoch', 'logical_scene_source_owner_epoch',
    'logical_scene_source_generation', 'scene_runtime_source_owner_epoch',
    'scene_runtime_source_generation', 'alarm_owner_epoch', 'alarm_generation',
    'alarm_state_revision', 'admission_generation', 'program_runner_owner_epoch',
    'program_cycle_generation', 'applied_program_owner_epoch',
    'applied_program_generation', 'execution_owner_epoch',
    'execution_publication_generation',
  ];
  exactKeysAt(basis, basisKeys, basisPath);
  const nullableGeneration = (name: string): number | null => (
    basis[name] === null ? null : integerAt(basis[name], `${basisPath}.${name}`)
  );
  const maintenanceOperation = record.maintenance_operation === null
    ? null
    : (() => {
      const operationPath = `${path}.maintenance_operation`;
      const operation = objectAt(record.maintenance_operation, operationPath);
      exactKeysAt(operation, [
        'apply_id', 'apply_generation', 'phase',
        'affected_runtime_unit_ids', 'reason_code',
      ], operationPath);
      const reasonCode = stringAt(
        operation.reason_code,
        `${operationPath}.reason_code`,
      );
      if (reasonCode !== 'planned_maintenance_in_progress') {
        return fail(
          `${operationPath}.reason_code`,
          'planned_maintenance_in_progress',
        );
      }
      const phase = stringAt(operation.phase, `${operationPath}.phase`);
      if (!MANAGER_APPLY_PHASES.has(phase as ManagerApplyPhase)) {
        return fail(
          `${operationPath}.phase`,
          'a supported Manager Apply inventory phase',
        );
      }
      return {
        applyId: stringAt(operation.apply_id, `${operationPath}.apply_id`),
        applyGeneration: positiveIntegerAt(
          operation.apply_generation,
          `${operationPath}.apply_generation`,
        ),
        phase: phase as ManagerApplyPhase,
        affectedRuntimeUnitIds: stringArrayAt(
          operation.affected_runtime_unit_ids,
          `${operationPath}.affected_runtime_unit_ids`,
        ),
        reasonCode: 'planned_maintenance_in_progress' as const,
      };
    })();
  return {
    schemaVersion,
    overviewRevision: digestAt(record.overview_revision, `${path}.overview_revision`),
    readiness: readiness as ComponentOverviewProjection['readiness'],
    summaryLabel: stringAt(record.summary_label, `${path}.summary_label`),
    profileLabel: stringAt(record.profile_label, `${path}.profile_label`),
    authorityLabel: stringAt(record.authority_label, `${path}.authority_label`),
    userMessage: stringAt(record.user_message, `${path}.user_message`),
    capabilities: stringArrayAt(record.capabilities, `${path}.capabilities`),
    components,
    maintenanceOperation,
    basis: {
      runtimeProjectionOwnerEpoch: nullableStringAt(
        basis.runtime_projection_owner_epoch,
        `${basisPath}.runtime_projection_owner_epoch`,
      ),
      runtimeProjectionGeneration: nullableGeneration('runtime_projection_generation'),
      managerSourceOwnerEpoch: nullableStringAt(
        basis.manager_source_owner_epoch,
        `${basisPath}.manager_source_owner_epoch`,
      ),
      logicalSceneSourceOwnerEpoch: nullableStringAt(
        basis.logical_scene_source_owner_epoch,
        `${basisPath}.logical_scene_source_owner_epoch`,
      ),
      logicalSceneSourceGeneration: nullableGeneration('logical_scene_source_generation'),
      sceneRuntimeSourceOwnerEpoch: nullableStringAt(
        basis.scene_runtime_source_owner_epoch,
        `${basisPath}.scene_runtime_source_owner_epoch`,
      ),
      sceneRuntimeSourceGeneration: nullableGeneration('scene_runtime_source_generation'),
      alarmOwnerEpoch: nullableStringAt(
        basis.alarm_owner_epoch,
        `${basisPath}.alarm_owner_epoch`,
      ),
      alarmGeneration: nullableGeneration('alarm_generation'),
      alarmStateRevision: nullableGeneration('alarm_state_revision'),
      admissionGeneration: nullableGeneration('admission_generation'),
      programRunnerOwnerEpoch: nullableStringAt(
        basis.program_runner_owner_epoch,
        `${basisPath}.program_runner_owner_epoch`,
      ),
      programCycleGeneration: nullableGeneration('program_cycle_generation'),
      appliedProgramOwnerEpoch: nullableStringAt(
        basis.applied_program_owner_epoch,
        `${basisPath}.applied_program_owner_epoch`,
      ),
      appliedProgramGeneration: nullableGeneration('applied_program_generation'),
      executionOwnerEpoch: nullableStringAt(
        basis.execution_owner_epoch,
        `${basisPath}.execution_owner_epoch`,
      ),
      executionPublicationGeneration: nullableGeneration(
        'execution_publication_generation',
      ),
    },
  };
}

export function parseComposerHeadlessState(
  value: unknown,
): ComposerHeadlessState {
  const record = objectAt(value, 'headlessState');
  exactKeysAt(record, [
    'schema_version', 'runtime', 'alarms', 'component_overview', 'authoring_admission',
    'held_run_draft_edit_allowed',
    'execution_targets', 'execution_control_state', 'program_cycle',
    'authoring_workspace', 'authoring_workspace_containment', 'receiver_epoch',
    'snapshot_sequence', 'complete',
  ], 'headlessState');
  const schemaVersion = stringAt(
    record.schema_version,
    'headlessState.schema_version',
  );
  if (schemaVersion !== 'manyforge.composer_headless_state.v2') {
    return fail(
      'headlessState.schema_version',
      'manyforge.composer_headless_state.v2',
    );
  }
  const admission = objectAt(record.authoring_admission, 'headlessState.authoring_admission');
  exactKeysAt(admission, ['admission_generation', 'authoring_allowed'], 'headlessState.authoring_admission');
  const complete = booleanAt(record.complete, 'headlessState.complete');
  if (!complete) return fail('headlessState.complete', 'true');
  return {
    schemaVersion,
    complete: true,
    runtime: parseHeadlessRuntimeSnapshot(record.runtime),
    alarms: parseOperatorAlarmSnapshot(record.alarms),
    componentOverview: parseComponentOverviewProjection(
      record.component_overview,
      'headlessState.component_overview',
    ),
    authoringAdmission: {
      authoringAllowed: booleanAt(admission.authoring_allowed, 'headlessState.authoring_admission.authoring_allowed'),
      admissionGeneration: positiveIntegerAt(admission.admission_generation, 'headlessState.authoring_admission.admission_generation'),
    },
    heldRunDraftEditAllowed: booleanAt(
      record.held_run_draft_edit_allowed,
      'headlessState.held_run_draft_edit_allowed',
    ),
    executionControlStatus: record.execution_targets === null
      && record.execution_control_state === null
      ? null
      : parseExecutionControlStatus(
          record.execution_targets,
          record.execution_control_state,
        ),
    programCycle: parseProgramCycleStatus(record.program_cycle),
    authoringWorkspace: parseAuthoringWorkspaceIdentity(
      record.authoring_workspace,
      'headlessState.authoring_workspace',
    ),
    authoringWorkspaceContainment: parseAuthoringWorkspaceContainment(
      record.authoring_workspace_containment,
      'headlessState.authoring_workspace_containment',
    ),
    receiverEpoch: stringAt(record.receiver_epoch, 'headlessState.receiver_epoch'),
    snapshotSequence: positiveIntegerAt(record.snapshot_sequence, 'headlessState.snapshot_sequence'),
  };
}

export function parseObjectResponse(value: unknown): Record<string, unknown> {
  return objectAt(value, 'response');
}

/**
 * Parse the complete stop-fault recovery offer used by the recovery command.
 * The owner supplies these facts; the UI never invents or caches them.
 */
export function parseStopFaultRecoveryOffer(
  value: unknown,
): StopFaultRecoveryOffer | null {
  const envelope = objectAt(value, 'stopFaultResponse');
  allowedKeysAt(
    envelope,
    ['api_version', 'command', 'ok', 'stop_fault'],
    ['global_rule_recovery_offers'],
    'stopFaultResponse',
  );
  if (
    stringAt(envelope.api_version, 'stopFaultResponse.api_version')
      !== 'manyforge.execution_command_response.v1'
    || booleanAt(envelope.ok, 'stopFaultResponse.ok') !== true
    || stringAt(envelope.command, 'stopFaultResponse.command') !== 'get_stop_fault'
  ) {
    return fail('stopFaultResponse', 'a successful stop-fault response');
  }
  if (envelope.stop_fault === null) return null;
  const offer = objectAt(envelope.stop_fault, 'stopFaultResponse.stop_fault');
  exactKeysAt(
    offer,
    ['configuration_expectations', 'latch', 'recovery_unavailable_reason'],
    'stopFaultResponse.stop_fault',
  );
  const latch = objectAt(offer.latch, 'stopFaultResponse.stop_fault.latch');
  allowedKeysAt(
    latch,
    [
      'cell_id', 'graph_revision', 'latch_generation', 'latch_id',
      'recovery_assessment_revision', 'recovery_status',
      'retained_bundle_id', 'state', 'stop_certificate',
    ],
    [
      'captured_operation_ids', 'captured_resources', 'clear_projection',
      'evidence_refs', 'observed_triggers', 'persistence_degraded', 'reason_code',
      'stop_generation', 'stop_id', 'terminal_projection', 'trigger',
    ],
    'stopFaultResponse.stop_fault.latch',
  );
  const recoveryStatus = stringAt(
    latch.recovery_status,
    'stopFaultResponse.stop_fault.latch.recovery_status',
  );
  const supportedRecoveryStatuses = [
    'waiting_for_observation', 'waiting_for_quiescence',
    'waiting_for_configuration', 'waiting_for_external_reconciliation',
    'waiting_for_durable_record', 'ready', 'recovery_pending',
  ] as const;
  if (!(supportedRecoveryStatuses as readonly string[]).includes(recoveryStatus)) {
    return fail(
      'stopFaultResponse.stop_fault.latch.recovery_status',
      'a supported stop-fault recovery status',
    );
  }
  const certificate = latch.stop_certificate === null
    ? null
    : (() => {
      const record = objectAt(
        latch.stop_certificate,
        'stopFaultResponse.stop_fault.latch.stop_certificate',
      );
      exactKeysAt(record, [
        'stop_certificate_id', 'stop_certificate_revision',
        'stop_generation', 'stop_id',
      ], 'stopFaultResponse.stop_fault.latch.stop_certificate');
      return {
        stopCertificateId: stringAt(
          record.stop_certificate_id,
          'stopFaultResponse.stop_fault.latch.stop_certificate.stop_certificate_id',
        ),
        stopCertificateRevision: positiveIntegerAt(
          record.stop_certificate_revision,
          'stopFaultResponse.stop_fault.latch.stop_certificate.stop_certificate_revision',
        ),
        stopGeneration: positiveIntegerAt(
          record.stop_generation,
          'stopFaultResponse.stop_fault.latch.stop_certificate.stop_generation',
        ),
        stopId: stringAt(
          record.stop_id,
          'stopFaultResponse.stop_fault.latch.stop_certificate.stop_id',
        ),
      };
    })();
  const expectations = arrayAt(
    offer.configuration_expectations,
    'stopFaultResponse.stop_fault.configuration_expectations',
  ).map((value, index) => {
    const path = `stopFaultResponse.stop_fault.configuration_expectations[${index}]`;
    const row = objectAt(value, path);
    exactKeysAt(row, [
      'active_control_configuration_generation',
      'active_control_configuration_id',
      'control_domain_id',
    ], path);
    return {
      controlDomainId: stringAt(row.control_domain_id, `${path}.control_domain_id`),
      activeControlConfigurationId: stringAt(
        row.active_control_configuration_id,
        `${path}.active_control_configuration_id`,
      ),
      activeControlConfigurationGeneration: positiveIntegerAt(
        row.active_control_configuration_generation,
        `${path}.active_control_configuration_generation`,
      ),
    };
  });
  const recoveryUnavailableReason = offer.recovery_unavailable_reason === null
    ? null
    : stringAt(
      offer.recovery_unavailable_reason,
      'stopFaultResponse.stop_fault.recovery_unavailable_reason',
    );
  // A certified latch carries a non-empty proof or a named unavailable reason,
  // never both; an uncertified latch carries neither.
  if (
    (certificate === null
      ? expectations.length !== 0
        || recoveryUnavailableReason !== null
        || recoveryStatus !== 'waiting_for_observation'
      : (expectations.length === 0) === (recoveryUnavailableReason === null))
    || expectations.some((row, index) => (
      index > 0
      && expectations[index - 1].controlDomainId >= row.controlDomainId
    ))
  ) {
    return fail(
      'stopFaultResponse.stop_fault.configuration_expectations',
      'valid configuration expectations sorted by control domain',
    );
  }
  return {
    latchId: stringAt(latch.latch_id, 'stopFaultResponse.stop_fault.latch.latch_id'),
    latchGeneration: positiveIntegerAt(
      latch.latch_generation,
      'stopFaultResponse.stop_fault.latch.latch_generation',
    ),
    recoveryAssessmentRevision: positiveIntegerAt(
      latch.recovery_assessment_revision,
      'stopFaultResponse.stop_fault.latch.recovery_assessment_revision',
    ),
    recoveryStatus: recoveryStatus as StopFaultRecoveryOffer['recoveryStatus'],
    cellId: stringAt(latch.cell_id, 'stopFaultResponse.stop_fault.latch.cell_id'),
    retainedBundleId: stringAt(
      latch.retained_bundle_id,
      'stopFaultResponse.stop_fault.latch.retained_bundle_id',
    ),
    graphRevision: stringAt(
      latch.graph_revision,
      'stopFaultResponse.stop_fault.latch.graph_revision',
    ),
    stopCertificate: certificate,
    configurationExpectations: expectations,
    recoveryUnavailableReason,
  };
}

function parseGlobalRuleContext(
  value: unknown,
  path: string,
): GlobalRuleRecoverySelector['context'] {
  const context = objectAt(value, path);
  exactKeysAt(context, [
    'active_publication_generation', 'bundle', 'catalog_generation', 'graph', 'owner_epoch',
  ], path);
  return {
    owner_epoch: stringAt(context.owner_epoch, `${path}.owner_epoch`),
    catalog_generation: nullablePositiveUint64TextAt(
      stringAt(context.catalog_generation, `${path}.catalog_generation`),
      `${path}.catalog_generation`,
    )!,
    active_publication_generation: positiveIntegerAt(
      context.active_publication_generation,
      `${path}.active_publication_generation`,
    ),
    bundle: stringAt(context.bundle, `${path}.bundle`),
    graph: stringAt(context.graph, `${path}.graph`),
  };
}

function parseGlobalRuleRecoverySelector(
  value: unknown,
  path: string,
): GlobalRuleRecoverySelector {
  const selector = objectAt(value, path);
  exactKeysAt(selector, ['context', 'expected_rule_registration_id', 'rule_id'], path);
  return {
    context: parseGlobalRuleContext(selector.context, `${path}.context`),
    rule_id: stringAt(selector.rule_id, `${path}.rule_id`),
    // Canonical positive uint64 text: Core emits and accepts only >= 1.
    expected_rule_registration_id: nullablePositiveUint64TextAt(
      stringAt(selector.expected_rule_registration_id, `${path}.expected_rule_registration_id`),
      `${path}.expected_rule_registration_id`,
    )!,
  };
}

/**
 * Parse the latched global rules' recovery offers of the same get_stop_fault
 * reply (SX-2206). An owner that does not project them yet offers none.
 */
export function parseGlobalRuleRecoveryOffers(value: unknown): GlobalRuleRecoveryOffer[] {
  parseStopFaultRecoveryOffer(value);
  const envelope = objectAt(value, 'stopFaultResponse');
  if (envelope.global_rule_recovery_offers === undefined) return [];
  return arrayAt(
    envelope.global_rule_recovery_offers,
    'stopFaultResponse.global_rule_recovery_offers',
  ).map((item, index) => {
    const path = `stopFaultResponse.global_rule_recovery_offers[${index}]`;
    const offer = objectAt(item, path);
    exactKeysAt(offer, [
      'current_condition', 'latch_cause', 'post_latch_disposition', 'selector',
    ], path);
    const currentCondition = offer.current_condition === null
      ? null : booleanAt(offer.current_condition, `${path}.current_condition`);
    const disposition = stringAt(offer.post_latch_disposition, `${path}.post_latch_disposition`);
    if (disposition !== 'auto_clear_on_signal_release' && disposition !== 'await_operator_ack') {
      return fail(`${path}.post_latch_disposition`, 'a supported post-latch disposition');
    }
    const cause = stringAt(offer.latch_cause, `${path}.latch_cause`);
    if (cause !== 'condition' && cause !== 'source_loss') {
      return fail(`${path}.latch_cause`, 'a supported latch cause');
    }
    return {
      selector: parseGlobalRuleRecoverySelector(offer.selector, `${path}.selector`),
      currentCondition,
      postLatchDisposition: disposition,
      latchCause: cause,
    };
  });
}

/**
 * Parse the owner result of one `global_io_rule` recover_stop_fault request.
 * It must echo the exact requested selector's rule and context (SX-2216),
 * the request's own context also for a replay after the Applied one moved.
 */
export function parseGlobalRuleRecoveryResult(
  value: unknown,
  expectedRequestId: string,
  expectedSelector: GlobalRuleRecoverySelector,
): GlobalRuleRecoveryResult {
  const expectedRuleId = expectedSelector.rule_id;
  const envelope = objectAt(value, 'globalRuleRecoveryResponse');
  exactKeysAt(envelope, ['api_version', 'command', 'ok', 'result'], 'globalRuleRecoveryResponse');
  if (
    stringAt(envelope.api_version, 'globalRuleRecoveryResponse.api_version')
      !== 'manyforge.execution_command_response.v1'
    || booleanAt(envelope.ok, 'globalRuleRecoveryResponse.ok') !== true
    || stringAt(envelope.command, 'globalRuleRecoveryResponse.command') !== 'recover_stop_fault'
  ) {
    return fail('globalRuleRecoveryResponse', 'a successful global-rule recovery response');
  }
  const path = 'globalRuleRecoveryResponse.result';
  const result = objectAt(envelope.result, path);
  exactKeysAt(result, [
    'accepted', 'context', 'error', 'replay_status', 'request_id', 'rule',
  ], path);
  if (stringAt(result.request_id, `${path}.request_id`) !== expectedRequestId) {
    return fail(`${path}.request_id`, `the request ID ${expectedRequestId}`);
  }
  const context = parseGlobalRuleContext(result.context, `${path}.context`);
  const expected = expectedSelector.context;
  if (
    context.owner_epoch !== expected.owner_epoch
    || context.catalog_generation !== expected.catalog_generation
    || context.active_publication_generation !== expected.active_publication_generation
    || context.bundle !== expected.bundle
    || context.graph !== expected.graph
  ) {
    return fail(`${path}.context`, 'the requested selector context');
  }
  const accepted = booleanAt(result.accepted, `${path}.accepted`);
  let ruleState: GlobalRuleRecoveryResult['ruleState'] = null;
  if (result.rule !== null || accepted) {
    const rule = objectAt(result.rule, `${path}.rule`);
    exactKeysAt(rule, ['rule_id', 'state', 'triggering_acquisition'], `${path}.rule`);
    if (stringAt(rule.rule_id, `${path}.rule.rule_id`) !== expectedRuleId) {
      return fail(`${path}.rule.rule_id`, `the rule ${expectedRuleId}`);
    }
    const state = stringAt(rule.state, `${path}.rule.state`);
    if (state !== 'clear' && state !== 'tripped' && state !== 'latched') {
      return fail(`${path}.rule.state`, 'a supported rule state');
    }
    ruleState = state;
  }
  return {
    accepted,
    replayStatus: nullableStringAt(result.replay_status, `${path}.replay_status`),
    ruleState,
    // The owner's refusal detail is shown verbatim; a structured one as JSON.
    error: result.error === null || typeof result.error === 'string'
      ? result.error : JSON.stringify(result.error),
  };
}

export function parseCellRecoverResult(
  value: unknown,
  expectedRequestId: string,
): CellRecoverResult {
  const path = 'cellRecoverResult';
  const result = objectAt(value, path);
  exactKeysAt(result, [
    'acknowledged_incidents', 'latch_cleared', 'outcome', 'reason', 'recovered_components',
    'request_id', 'schema_version',
  ], path);
  if (stringAt(result.schema_version, `${path}.schema_version`) !== 'manyforge.cell_recover_result.v1') {
    return fail(`${path}.schema_version`, 'manyforge.cell_recover_result.v1');
  }
  if (stringAt(result.request_id, `${path}.request_id`) !== expectedRequestId) {
    return fail(`${path}.request_id`, `the request ID ${expectedRequestId}`);
  }
  const outcome = stringAt(result.outcome, `${path}.outcome`);
  if (outcome !== 'done' && outcome !== 'not_recovered') {
    return fail(`${path}.outcome`, 'done or not_recovered');
  }
  const acknowledgedIncidents = arrayAt(result.acknowledged_incidents, `${path}.acknowledged_incidents`)
    .map((entry, index) => {
      const at = `${path}.acknowledged_incidents[${index}]`;
      const row = objectAt(entry, at);
      exactKeysAt(row, ['cause_class', 'controllable_component_id', 'incident_id'], at);
      return {
        controllableComponentId: stringAt(row.controllable_component_id, `${at}.controllable_component_id`),
        incidentId: stringAt(row.incident_id, `${at}.incident_id`),
        causeClass: stringAt(row.cause_class, `${at}.cause_class`),
      };
    });
  let reason: CellRecoverResult['reason'] = null;
  if (result.reason !== null) {
    const at = `${path}.reason`;
    const row = objectAt(result.reason, at);
    exactKeysAt(row, ['code', 'details', 'message'], at);
    const details = objectAt(row.details, `${at}.details`);
    reason = {
      code: stringAt(row.code, `${at}.code`),
      message: stringAt(row.message, `${at}.message`),
      details: Object.fromEntries(Object.entries(details).map(
        ([key, detail]) => [key, stringAt(detail, `${at}.details.${key}`)],
      )),
    };
  }
  if ((outcome === 'done') !== (reason === null)) {
    return fail(`${path}.reason`, 'a reason exactly when not recovered');
  }
  return {
    outcome,
    acknowledgedIncidents,
    latchCleared: booleanAt(result.latch_cleared, `${path}.latch_cleared`),
    recoveredComponents: arrayAt(result.recovered_components, `${path}.recovered_components`)
      .map((id, index) => stringAt(id, `${path}.recovered_components[${index}]`)),
    reason,
  };
}
