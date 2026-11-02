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
import {
  arrayAt,
  booleanAt,
  boundedArrayAt,
  digestAt,
  exactKeysAt,
  fail,
  integerAt,
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

