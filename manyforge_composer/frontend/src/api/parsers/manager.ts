import type {
  PreparedActivationInput,
  PreparedActivationOrigin,
  ManagerActiveProjection,
  ManagerApplyAvailability,
  ManagerApplyFailure,
  ManagerApplyLifecyclePhase,
  ManagerApplyPhase,
  ManagerApplyRecord,
  ManagerApplyReplayStatus,
  ManagerApplyResponse,
  ManagerApplyStatus,
  ManagerDeploymentCandidatePreviewResponse,
  ManagerDeploymentImpact,
  ManagerDeploymentImpactComponent,
  ManagerDeploymentRecordDiff,
  ProgramUnresolvedReference,
  AppliedProgramStatus,
  ManagerDiffProjection,
  ManagerGraphProjection,
  ManagerRuntimeProjection,
  ManagerSourceMetadata,
  ManagerStatusProjection,
} from '../../types/api';
import type { JsonObject } from './shared';
import {
  booleanAt,
  boundedArrayAt,
  digestAt,
  exactKeysAt,
  fail,
  integerAt,
  jsonRecordAt,
  jsonValueAt,
  nonEmptyStringAt,
  nullableNumberAt,
  nullableStringAt,
  objectAt,
  parseProjectionRevision,
  positiveIntegerAt,
  sourceFreshnessAt,
  stringArrayAt,
  stringAt,
} from './shared';

export function parseRuntimeAvailability(value: unknown, path: string) {
  const record = objectAt(value, path);
  exactKeysAt(record, ['code', 'message', 'restorationGates'], path);
  return {
    code: stringAt(record.code, `${path}.code`),
    message: stringAt(record.message, `${path}.message`),
    restorationGates: stringArrayAt(record.restorationGates, `${path}.restorationGates`),
  };
}

function parseManagerSourceMetadata(
  value: unknown,
  path: string,
): ManagerSourceMetadata {
  const record = objectAt(value, path);
  exactKeysAt(
    record,
    [
      'available',
      'freshness',
      'known_gaps',
      'projection_generation',
      'projection_owner_epoch',
      'receiver_age_ns',
      'source_content_digest',
      'source_generation',
      'source_owner_epoch',
      'source_owner_id',
    ],
    path,
  );
  return {
    projectionGeneration: integerAt(
      record.projection_generation,
      path + '.projection_generation',
    ),
    projectionOwnerEpoch: stringAt(
      record.projection_owner_epoch,
      path + '.projection_owner_epoch',
    ),
    available: booleanAt(record.available, path + '.available'),
    freshness: sourceFreshnessAt(record.freshness, path + '.freshness'),
    knownGaps: stringArrayAt(record.known_gaps, path + '.known_gaps'),
    receiverAgeNs: nullableNumberAt(
      record.receiver_age_ns,
      path + '.receiver_age_ns',
    ),
    sourceGeneration: nullableNumberAt(
      record.source_generation,
      path + '.source_generation',
    ),
    sourceOwnerEpoch: nullableStringAt(
      record.source_owner_epoch,
      path + '.source_owner_epoch',
    ),
    sourceOwnerId: nullableStringAt(
      record.source_owner_id,
      path + '.source_owner_id',
    ),
    sourceContentDigest: nullableStringAt(
      record.source_content_digest,
      path + '.source_content_digest',
    ),
  };
}

function parseManagerActiveProjection(
  value: unknown,
  path: string,
): ManagerActiveProjection {
  const record = objectAt(value, path);
  exactKeysAt(
    record,
    ['canonical_empty', 'cell_graph', 'graph_revision', 'publication', 'retained_bundle_id'],
    path,
  );
  return {
    canonicalEmpty: booleanAt(record.canonical_empty, path + '.canonical_empty'),
    cellGraph: jsonRecordAt(record.cell_graph, path + '.cell_graph'),
    graphRevision: stringAt(record.graph_revision, path + '.graph_revision'),
    retainedBundleId: stringAt(
      record.retained_bundle_id,
      path + '.retained_bundle_id',
    ),
    publication: jsonRecordAt(record.publication, path + '.publication'),
  };
}

export function parseManagerApplyAvailability(
  value: unknown,
  path: string,
): ManagerApplyAvailability {
  const record = objectAt(value, path);
  exactKeysAt(
    record,
    ['mutation_available', 'records', 'unavailable_reason_code'],
    path,
  );
  return {
    mutationAvailable: booleanAt(
      record.mutation_available,
      path + '.mutation_available',
    ),
    records: boundedArrayAt(record.records, path + '.records', 4096).map(
      (item, index) => parseManagerApplyRecord(
        item,
        path + '.records[' + index + ']',
      ),
    ),
    unavailableReasonCode: nullableStringAt(
      record.unavailable_reason_code,
      path + '.unavailable_reason_code',
    ),
  };
}

export const MANAGER_APPLY_PHASES = new Set<ManagerApplyPhase>([
  'intent_persisted',
  'scope_persisted',
  'fence_acquire_pending',
  'fence_held',
  'runtime_convergence_pending',
  'runtime_staged_ready',
  'scene_stage_pending',
  'scene_staged',
  'activation_selected',
  'scene_activated',
  'private_materialized',
  'command_surfaces_retired',
  'active_bundle_published',
  'scene_finalized',
  'fence_release_pending',
  'fence_released',
  'terminal',
]);

const MANAGER_FENCE_STATES = new Set<ManagerApplyRecord['fence']['state']>([
  'not_requested',
  'scope_persisted',
  'acquire_pending',
  'held',
  'release_pending',
  'released',
]);

function parseSceneInitialization(value: unknown, path: string): 'preserve' | 'initialize' {
  if (value !== 'preserve' && value !== 'initialize') {
    return fail(path, 'preserve or initialize');
  }
  return value;
}

function parseManagerApplyRecord(
  value: unknown,
  path: string,
): ManagerApplyRecord {
  const record = objectAt(value, path);
  exactKeysAt(
    record,
    [
      'apply_generation',
      'apply_id',
      'candidate_graph_revision',
      'candidate_retained_bundle_id',
      'committed',
      'expected_active_projection_revision',
      'fence',
      'incomplete',
      'intent_digest',
      'phase',
      'prepared_activation',
      'scene_initialization',
      'request',
      'terminal_state',
    ],
    path,
  );
  // Status publishes the durable preparation receipt, even though this view
  // only projects Apply progress. Validate it through the shared v3 contract.
  parsePreparedActivationInput(record.prepared_activation, path + '.prepared_activation');
  const phase = stringAt(record.phase, path + '.phase');
  if (!MANAGER_APPLY_PHASES.has(phase as ManagerApplyPhase)) {
    return fail(path + '.phase', 'a supported Manager Apply phase');
  }
  const fence = objectAt(record.fence, path + '.fence');
  exactKeysAt(
    fence,
    [
      'acceptance_digest',
      'configuration_fence_generation',
      'configuration_fence_id',
      'fence_attempt_digest',
      'scope',
      'state',
    ],
    path + '.fence',
  );
  const fenceState = stringAt(fence.state, path + '.fence.state');
  if (
    !MANAGER_FENCE_STATES.has(
      fenceState as ManagerApplyRecord['fence']['state'],
    )
  ) {
    return fail(path + '.fence.state', 'a supported Manager fence state');
  }
  const fenceGeneration = fence.configuration_fence_generation === null
    ? null
    : positiveIntegerAt(
        fence.configuration_fence_generation,
        path + '.fence.configuration_fence_generation',
      );
  const scope = fence.scope === null
    ? null
    : jsonRecordAt(fence.scope, path + '.fence.scope');
  const request = objectAt(record.request, path + '.request');
  exactKeysAt(
    request,
    ['correlation_id', 'request_id', 'request_owner_epoch', 'source_label'],
    path + '.request',
  );
  const terminalState = nullableStringAt(
    record.terminal_state,
    path + '.terminal_state',
  );
  if (
    terminalState !== null
    && !['succeeded', 'failed', 'unknown_effect'].includes(terminalState)
  ) {
    return fail(
      path + '.terminal_state',
      'succeeded, failed, unknown_effect, or null',
    );
  }
  const incomplete = booleanAt(record.incomplete, path + '.incomplete');
  if (incomplete !== (terminalState === null)) {
    return fail(
      path + '.incomplete',
      'true exactly while the operation has no terminal state',
    );
  }
  return {
    applyGeneration: positiveIntegerAt(
      record.apply_generation,
      path + '.apply_generation',
    ),
    applyId: stringAt(record.apply_id, path + '.apply_id'),
    sceneInitialization: parseSceneInitialization(record.scene_initialization, path + '.scene_initialization'),
    candidateGraphRevision: stringAt(
      record.candidate_graph_revision,
      path + '.candidate_graph_revision',
    ),
    candidateRetainedBundleId: stringAt(
      record.candidate_retained_bundle_id,
      path + '.candidate_retained_bundle_id',
    ),
    committed: booleanAt(record.committed, path + '.committed'),
    expectedActiveProjectionRevision: parseProjectionRevision(
      record.expected_active_projection_revision,
      path + '.expected_active_projection_revision',
    ),
    fence: {
      acceptanceDigest: nullableStringAt(
        fence.acceptance_digest,
        path + '.fence.acceptance_digest',
      ),
      configurationFenceGeneration: fenceGeneration,
      configurationFenceId: nullableStringAt(
        fence.configuration_fence_id,
        path + '.fence.configuration_fence_id',
      ),
      fenceAttemptDigest: nullableStringAt(
        fence.fence_attempt_digest,
        path + '.fence.fence_attempt_digest',
      ),
      scope,
      state: fenceState as ManagerApplyRecord['fence']['state'],
    },
    incomplete,
    intentDigest: stringAt(record.intent_digest, path + '.intent_digest'),
    phase: phase as ManagerApplyPhase,
    request: {
      correlationId: nullableStringAt(
        request.correlation_id,
        path + '.request.correlation_id',
      ),
      requestId: stringAt(request.request_id, path + '.request.request_id'),
      requestOwnerEpoch: stringAt(
        request.request_owner_epoch,
        path + '.request.request_owner_epoch',
      ),
      sourceLabel: stringAt(request.source_label, path + '.request.source_label'),
    },
    terminalState: terminalState as ManagerApplyRecord['terminalState'],
  };
}

function parseManagerRuntimeProjection(
  value: unknown,
  path: string,
): ManagerRuntimeProjection {
  const record = objectAt(value, path);
  exactKeysAt(record, ['converged', 'desired_units', 'observed_units'], path);
  return {
    converged: booleanAt(record.converged, path + '.converged'),
    desiredUnits: boundedArrayAt(
      record.desired_units,
      path + '.desired_units',
      4096,
    ).map((item, index) => jsonValueAt(item, path + '.desired_units[' + index + ']')),
    observedUnits: boundedArrayAt(
      record.observed_units,
      path + '.observed_units',
      4096,
    ).map((item, index) => jsonValueAt(item, path + '.observed_units[' + index + ']')),
  };
}

export function parseManagerStatusProjection(
  value: unknown,
): ManagerStatusProjection {
  const record = objectAt(value, 'managerStatus');
  exactKeysAt(record, ['schema_version', 'source', 'status'], 'managerStatus');
  const schemaVersion = stringAt(
    record.schema_version,
    'managerStatus.schema_version',
  );
  if (schemaVersion !== 'manyforge.manager_status_projection.v1') {
    return fail(
      'managerStatus.schema_version',
      'manyforge.manager_status_projection.v1',
    );
  }
  const status = objectAt(record.status, 'managerStatus.status');
  exactKeysAt(
    status,
    ['active', 'api_version', 'apply', 'cell_id', 'manager_ready', 'runtime'],
    'managerStatus.status',
  );
  const apiVersion = stringAt(
    status.api_version,
    'managerStatus.status.api_version',
  );
  if (apiVersion !== 'manyforge.manager_source.v1') {
    return fail(
      'managerStatus.status.api_version',
      'manyforge.manager_source.v1',
    );
  }
  return {
    schemaVersion,
    source: parseManagerSourceMetadata(record.source, 'managerStatus.source'),
    status: {
      apiVersion,
      active: parseManagerActiveProjection(
        status.active,
        'managerStatus.status.active',
      ),
      apply: parseManagerApplyAvailability(
        status.apply,
        'managerStatus.status.apply',
      ),
      cellId: stringAt(status.cell_id, 'managerStatus.status.cell_id'),
      managerReady: booleanAt(
        status.manager_ready,
        'managerStatus.status.manager_ready',
      ),
      runtime: parseManagerRuntimeProjection(
        status.runtime,
        'managerStatus.status.runtime',
      ),
    },
  };
}

export function parseManagerGraphProjection(
  value: unknown,
): ManagerGraphProjection {
  const record = objectAt(value, 'managerGraph');
  exactKeysAt(
    record,
    ['cell_graph', 'graph_revision', 'retained_bundle_id', 'schema_version', 'source'],
    'managerGraph',
  );
  const schemaVersion = stringAt(
    record.schema_version,
    'managerGraph.schema_version',
  );
  if (schemaVersion !== 'manyforge.manager_graph_projection.v1') {
    return fail(
      'managerGraph.schema_version',
      'manyforge.manager_graph_projection.v1',
    );
  }
  return {
    schemaVersion,
    source: parseManagerSourceMetadata(record.source, 'managerGraph.source'),
    graphRevision: stringAt(record.graph_revision, 'managerGraph.graph_revision'),
    retainedBundleId: stringAt(
      record.retained_bundle_id,
      'managerGraph.retained_bundle_id',
    ),
    cellGraph: jsonRecordAt(record.cell_graph, 'managerGraph.cell_graph'),
  };
}

export function parseManagerDiffProjection(
  value: unknown,
): ManagerDiffProjection {
  const record = objectAt(value, 'managerDiff');
  exactKeysAt(
    record,
    ['converged', 'desired_units', 'observed_units', 'schema_version', 'source'],
    'managerDiff',
  );
  const schemaVersion = stringAt(
    record.schema_version,
    'managerDiff.schema_version',
  );
  if (schemaVersion !== 'manyforge.manager_diff_projection.v1') {
    return fail(
      'managerDiff.schema_version',
      'manyforge.manager_diff_projection.v1',
    );
  }
  return {
    schemaVersion,
    source: parseManagerSourceMetadata(record.source, 'managerDiff.source'),
    converged: booleanAt(record.converged, 'managerDiff.converged'),
    desiredUnits: boundedArrayAt(
      record.desired_units,
      'managerDiff.desired_units',
      4096,
    ).map((item, index) => jsonValueAt(item, 'managerDiff.desired_units[' + index + ']')),
    observedUnits: boundedArrayAt(
      record.observed_units,
      'managerDiff.observed_units',
      4096,
    ).map((item, index) => jsonValueAt(item, 'managerDiff.observed_units[' + index + ']')),
  };
}

export function parseManagerDeploymentCandidatePreviewResponse(
  value: unknown,
): ManagerDeploymentCandidatePreviewResponse {
  const record = objectAt(value, 'managerDeploymentCandidatePreview');
  exactKeysAt(
    record,
    ['api_version', 'candidate_receipt', 'converged', 'diff', 'impact', 'ok'],
    'managerDeploymentCandidatePreview',
  );
  if (record.ok !== true) {
    return fail('managerDeploymentCandidatePreview.ok', 'true');
  }
  const apiVersion = stringAt(
    record.api_version,
    'managerDeploymentCandidatePreview.api_version',
  );
  if (apiVersion !== 'manyforge.manager_command_response.v1') {
    return fail(
      'managerDeploymentCandidatePreview.api_version',
      'manyforge.manager_command_response.v1',
    );
  }
  const receipt = objectAt(
    record.candidate_receipt,
    'managerDeploymentCandidatePreview.candidate_receipt',
  );
  exactKeysAt(
    receipt,
    [
      'candidate_graph_revision',
      'candidate_retained_bundle_id',
      'deployment_artifact_id',
      'deployment_revision',
      'expected_admission_generation',
      'destructive_impact',
      'expected_current',
      'impact_digest',
      'manager_owner_epoch',
      'prepared_activation_id',
      'receipt_digest',
      'schema_version',
    ],
    'managerDeploymentCandidatePreview.candidate_receipt',
  );
  const schemaVersion = stringAt(
    receipt.schema_version,
    'managerDeploymentCandidatePreview.candidate_receipt.schema_version',
  );
  if (schemaVersion !== 'manyforge.manager_deployment_candidate_receipt.v2') {
    return fail(
      'managerDeploymentCandidatePreview.candidate_receipt.schema_version',
      'manyforge.manager_deployment_candidate_receipt.v2',
    );
  }
  const expected = objectAt(
    receipt.expected_current,
    'managerDeploymentCandidatePreview.candidate_receipt.expected_current',
  );
  exactKeysAt(
    expected,
    ['graph_revision', 'retained_bundle_id'],
    'managerDeploymentCandidatePreview.candidate_receipt.expected_current',
  );
  const diff = objectAt(record.diff, 'managerDeploymentCandidatePreview.diff');
  const recordKinds = [
    'component_attachments',
    'controllable_components',
    'control_bindings',
    'control_domains',
    'model_instances',
    'runtime_units',
  ] as const;
  exactKeysAt(diff, [...recordKinds], 'managerDeploymentCandidatePreview.diff');
  const parsedDiff: Record<string, ManagerDeploymentRecordDiff> = {};
  for (const kind of recordKinds) {
    const row = objectAt(diff[kind], `managerDeploymentCandidatePreview.diff.${kind}`);
    exactKeysAt(
      row,
      ['added', 'changed', 'removed'],
      `managerDeploymentCandidatePreview.diff.${kind}`,
    );
    const strings = (field: 'added' | 'changed' | 'removed') => boundedArrayAt(
      row[field],
      `managerDeploymentCandidatePreview.diff.${kind}.${field}`,
      4096,
    ).map((item, index) => stringAt(
      item,
      `managerDeploymentCandidatePreview.diff.${kind}.${field}[${index}]`,
    ));
    parsedDiff[kind] = {
      added: strings('added'),
      changed: strings('changed'),
      removed: strings('removed'),
    };
  }
  return {
    apiVersion,
    candidateReceipt: {
      schema_version: schemaVersion,
      deployment_artifact_id: stringAt(receipt.deployment_artifact_id, 'candidateReceipt.deployment_artifact_id'),
      deployment_revision: digestAt(receipt.deployment_revision, 'candidateReceipt.deployment_revision'),
      prepared_activation_id: digestAt(receipt.prepared_activation_id, 'candidateReceipt.prepared_activation_id'),
      candidate_retained_bundle_id: stringAt(receipt.candidate_retained_bundle_id, 'candidateReceipt.candidate_retained_bundle_id'),
      candidate_graph_revision: stringAt(receipt.candidate_graph_revision, 'candidateReceipt.candidate_graph_revision'),
      expected_current: {
        graph_revision: stringAt(expected.graph_revision, 'candidateReceipt.expected_current.graph_revision'),
        retained_bundle_id: stringAt(expected.retained_bundle_id, 'candidateReceipt.expected_current.retained_bundle_id'),
      },
      expected_admission_generation: positiveIntegerAt(receipt.expected_admission_generation, 'candidateReceipt.expected_admission_generation'),
      manager_owner_epoch: stringAt(receipt.manager_owner_epoch, 'candidateReceipt.manager_owner_epoch'),
      impact_digest: digestAt(receipt.impact_digest, 'candidateReceipt.impact_digest'),
      destructive_impact: booleanAt(receipt.destructive_impact, 'candidateReceipt.destructive_impact'),
      receipt_digest: digestAt(receipt.receipt_digest, 'candidateReceipt.receipt_digest'),
    },
    converged: booleanAt(record.converged, 'managerDeploymentCandidatePreview.converged'),
    diff: parsedDiff,
    impact: parseManagerDeploymentImpact(record.impact, 'managerDeploymentCandidatePreview.impact'),
  };
}

/** The non-executable outcome's named references (shared by impact/status). */
function programUnresolvedReferencesAt(
  outcome: JsonObject,
  outcomePath: string,
): {
  unresolvedReferences: ProgramUnresolvedReference[];
  affectedNodes: string[];
  affectedExecutionGroups: string[];
} {
  const strings = (item: unknown, itemPath: string) => boundedArrayAt(item, itemPath, 4096)
    .map((entry, index) => stringAt(entry, `${itemPath}[${index}]`));
  const unresolvedReferences = boundedArrayAt(
    outcome.unresolved_roles,
    `${outcomePath}.unresolved_roles`,
    256,
  ).map((entry, index) => {
    const rowPath = `${outcomePath}.unresolved_roles[${index}]`;
    const row = objectAt(entry, rowPath);
    exactKeysAt(
      row,
      [
        'command_target_id', 'component_role_id', 'controllable_component_id',
        'detail', 'reason', 'target_role_id',
      ],
      rowPath,
    );
    return {
      component_role_id: stringAt(row.component_role_id, `${rowPath}.component_role_id`),
      target_role_id: nullableStringAt(row.target_role_id, `${rowPath}.target_role_id`),
      controllable_component_id: stringAt(
        row.controllable_component_id,
        `${rowPath}.controllable_component_id`,
      ),
      command_target_id: nullableStringAt(row.command_target_id, `${rowPath}.command_target_id`),
      reason: stringAt(row.reason, `${rowPath}.reason`),
      detail: stringAt(row.detail, `${rowPath}.detail`),
    };
  });
  if (unresolvedReferences.length === 0) {
    return fail(`${outcomePath}.unresolved_roles`, 'at least one named reference');
  }
  return {
    unresolvedReferences,
    affectedNodes: strings(outcome.affected_nodes, `${outcomePath}.affected_nodes`),
    affectedExecutionGroups: strings(
      outcome.affected_execution_groups,
      `${outcomePath}.affected_execution_groups`,
    ),
  };
}

export function parseAppliedProgramStatus(value: unknown): AppliedProgramStatus {
  const path = 'appliedProgramStatus';
  const record = objectAt(value, path);
  exactKeysAt(record, [
    'deployment_artifact_id', 'deployment_revision', 'executable',
    'graph_revision', 'message', 'non_executable', 'program',
    'program_binding_id', 'program_id', 'program_revision', 'record_digest',
    'retained_bundle_id', 'origin', 'schema_version',
  ], path);
  const schemaVersion = stringAt(record.schema_version, `${path}.schema_version`);
  if (schemaVersion !== 'manyforge.applied_program_status.v2') {
    return fail(`${path}.schema_version`, 'manyforge.applied_program_status.v2');
  }
  const origin = parsePreparedActivationOrigin(record.origin, `${path}.origin`);
  const executable = booleanAt(record.executable, `${path}.executable`);
  const noProgram = record.program_id === null;
  if ((noProgram && (schemaVersion !== 'manyforge.applied_program_status.v2'
      || executable || record.non_executable !== null
      || record.program_revision !== null || record.program !== null))
    || (!noProgram && executable !== (record.non_executable === null))
    || executable !== (record.program_binding_id !== null)) {
    return fail(path, 'a binding exactly when executable, named references otherwise');
  }
  if (origin.kind === 'sealed_canonical_empty' && (!noProgram
    || origin.deploymentArtifactId !== record.deployment_artifact_id
    || origin.deploymentRevision !== record.deployment_revision
    || origin.retainedBundleId !== record.retained_bundle_id
    || origin.graphRevision !== record.graph_revision)) {
    return fail(path, 'sealed empty origin matching its exact no-Program status');
  }
  const references = record.non_executable === null
    ? { unresolvedReferences: [], affectedNodes: [], affectedExecutionGroups: [] }
    : programUnresolvedReferencesAt(
      objectAt(record.non_executable, `${path}.non_executable`),
      `${path}.non_executable`,
    );
  return {
    deploymentArtifactId: stringAt(
      record.deployment_artifact_id,
      `${path}.deployment_artifact_id`,
    ),
    deploymentRevision: digestAt(
      record.deployment_revision,
      `${path}.deployment_revision`,
    ),
    executable,
    programId: noProgram ? null : stringAt(record.program_id, `${path}.program_id`),
    programRevision: noProgram ? null : digestAt(record.program_revision, `${path}.program_revision`),
    origin,
    message: nullableStringAt(record.message, `${path}.message`),
    ...references,
  };
}

const IMPACT_DISPOSITIONS = ['retain', 'add', 'remove', 'replace'] as const;

function parseManagerDeploymentImpact(
  value: unknown,
  path: string,
): ManagerDeploymentImpact {
  const record = objectAt(value, path);
  exactKeysAt(
    record,
    [
      'attachment_changed_components', 'component_delta_digest', 'components',
      'destructive', 'detached_components', 'program',
      'shared_scene_projection_changed',
    ],
    path,
  );
  const strings = (item: unknown, itemPath: string) => boundedArrayAt(item, itemPath, 4096)
    .map((entry, index) => stringAt(entry, `${itemPath}[${index}]`));
  const components = boundedArrayAt(record.components, `${path}.components`, 128)
    .map((entry, index) => {
      const rowPath = `${path}.components[${index}]`;
      const row = objectAt(entry, rowPath);
      exactKeysAt(
        row,
        ['controllable_component_id', 'disposition', 'replacement_reasons'],
        rowPath,
      );
      const disposition = stringAt(row.disposition, `${rowPath}.disposition`);
      if (!(IMPACT_DISPOSITIONS as readonly string[]).includes(disposition)) {
        return fail(`${rowPath}.disposition`, 'retain, add, remove, or replace');
      }
      return {
        controllable_component_id: stringAt(
          row.controllable_component_id,
          `${rowPath}.controllable_component_id`,
        ),
        disposition: disposition as ManagerDeploymentImpactComponent['disposition'],
        replacement_reasons: strings(row.replacement_reasons, `${rowPath}.replacement_reasons`),
      };
    });
  const program = objectAt(record.program, `${path}.program`);
  exactKeysAt(
    program,
    ['executable', 'non_executable', 'program_id', 'program_revision'],
    `${path}.program`,
  );
  const executable = booleanAt(program.executable, `${path}.program.executable`);
  let unresolvedReferences: ProgramUnresolvedReference[] = [];
  let affectedNodes: string[] = [];
  let affectedExecutionGroups: string[] = [];
  const noProgram = program.program_id === null;
  if ((noProgram && (executable || program.non_executable !== null || program.program_revision !== null))
    || (!noProgram && executable !== (program.non_executable === null))) {
    return fail(`${path}.program.non_executable`, 'present exactly for a non-executable Program');
  }
  if (program.non_executable !== null) {
    const outcomePath = `${path}.program.non_executable`;
    ({ unresolvedReferences, affectedNodes, affectedExecutionGroups } =
      programUnresolvedReferencesAt(objectAt(program.non_executable, outcomePath), outcomePath));
  }
  return {
    destructive: booleanAt(record.destructive, `${path}.destructive`),
    components,
    attachmentChangedComponents: strings(
      record.attachment_changed_components,
      `${path}.attachment_changed_components`,
    ),
    detachedComponents: strings(record.detached_components, `${path}.detached_components`),
    sharedSceneProjectionChanged: booleanAt(
      record.shared_scene_projection_changed,
      `${path}.shared_scene_projection_changed`,
    ),
    componentDeltaDigest: digestAt(record.component_delta_digest, `${path}.component_delta_digest`),
    program: {
      executable,
      programId: noProgram ? null : stringAt(program.program_id, `${path}.program.program_id`),
      programRevision: noProgram ? null : digestAt(program.program_revision, `${path}.program.program_revision`),
      unresolvedReferences,
      affectedNodes,
      affectedExecutionGroups,
    },
  };
}

export function parseManagerApplyResponse(value: unknown): ManagerApplyResponse {
  const record = objectAt(value, 'managerApply');
  exactKeysAt(
    record,
    [
      'api_version',
      'apply_generation',
      'apply_id',
      'candidate_projection_revision',
      'error',
      'ok',
      'replay_status',
      'request_id',
      'request_owner_epoch',
      'state',
    ],
    'managerApply',
  );
  const apiVersion = stringAt(record.api_version, 'managerApply.api_version');
  if (apiVersion !== 'manyforge.manager_command_response.v1') {
    return fail(
      'managerApply.api_version',
      'manyforge.manager_command_response.v1',
    );
  }
  const ok = booleanAt(record.ok, 'managerApply.ok');
  if (!ok) {
    return fail('managerApply.ok', 'true for an accepted Apply');
  }
  if (record.error !== null) {
    return fail('managerApply.error', 'null for an accepted Apply');
  }
  const replayStatus = stringAt(
    record.replay_status,
    'managerApply.replay_status',
  );
  if (replayStatus !== 'accepted_new' && replayStatus !== 'replayed_exact') {
    return fail(
      'managerApply.replay_status',
      'accepted_new or replayed_exact',
    );
  }
  const state = stringAt(record.state, 'managerApply.state');
  if (!['pending', 'succeeded', 'failed', 'unknown_effect'].includes(state)) {
    return fail(
      'managerApply.state',
      'pending, succeeded, failed, or unknown_effect',
    );
  }
  const applyId = stringAt(record.apply_id, 'managerApply.apply_id');
  if (applyId.length === 0) {
    return fail('managerApply.apply_id', 'a non-empty Apply identity');
  }
  const requestId = stringAt(record.request_id, 'managerApply.request_id');
  if (requestId.length === 0) {
    return fail('managerApply.request_id', 'a non-empty request identity');
  }
  const requestOwnerEpoch = stringAt(
    record.request_owner_epoch,
    'managerApply.request_owner_epoch',
  );
  if (requestOwnerEpoch.length === 0) {
    return fail(
      'managerApply.request_owner_epoch',
      'a non-empty request-owner epoch',
    );
  }
  const base = {
    apiVersion: 'manyforge.manager_command_response.v1' as const,
    applyGeneration: positiveIntegerAt(
      record.apply_generation,
      'managerApply.apply_generation',
    ),
    applyId,
    error: null,
    ok: true as const,
    replayStatus: replayStatus as ManagerApplyReplayStatus,
    requestId,
    requestOwnerEpoch,
  };
  if (state !== 'succeeded') {
    if (record.candidate_projection_revision !== null) {
      return fail(
        'managerApply.candidate_projection_revision',
        'null unless Apply succeeded',
      );
    }
    return {
      ...base,
      candidateProjectionRevision: null,
      state: state as 'pending' | 'failed' | 'unknown_effect',
    };
  }
  if (record.candidate_projection_revision === null) {
    return fail(
      'managerApply.candidate_projection_revision',
      'a projection revision while Apply has succeeded',
    );
  }
  return {
    ...base,
    candidateProjectionRevision: parseProjectionRevision(
      record.candidate_projection_revision,
      'managerApply.candidate_projection_revision',
    ),
    state: 'succeeded',
  };
}

const MANAGER_APPLY_LIFECYCLE_PHASES = new Set<ManagerApplyLifecyclePhase>([
  'accepted',
  'fence_acquired',
  'fence_accepted',
  'runtime_converged',
  'scene_staged',
  'scene_activated',
  'composed',
  'published',
  'settled',
]);

function managerApplyLifecyclePhaseAt(
  value: unknown,
  path: string,
): ManagerApplyLifecyclePhase {
  const phase = stringAt(value, path) as ManagerApplyLifecyclePhase;
  if (!MANAGER_APPLY_LIFECYCLE_PHASES.has(phase)) {
    return fail(path, 'a supported Manager Apply lifecycle phase');
  }
  return phase;
}

export function parseManagerApplyStatus(value: unknown): ManagerApplyStatus {
  const boundedText = (
    text: unknown,
    path: string,
    maximumBytes: number,
  ): string => {
    const parsed = nonEmptyStringAt(text, path);
    if (
      parsed !== parsed.normalize('NFC')
      || new TextEncoder().encode(parsed).length > maximumBytes
    ) {
      return fail(path, `nonempty NFC text of at most ${maximumBytes} bytes`);
    }
    return parsed;
  };
  const record = objectAt(value, 'managerApplyStatus');
  exactKeysAt(record, [
    'api_version', 'apply_generation', 'apply_id',
    'candidate_projection_revision', 'error', 'failure', 'last_progress',
    'ok', 'phase', 'prepared_activation', 'request_id',
    'request_owner_epoch', 'state', 'scene_initialization',
  ], 'managerApplyStatus');
  const apiVersion = stringAt(
    record.api_version,
    'managerApplyStatus.api_version',
  );
  if (apiVersion !== 'manyforge.manager_command_response.v1') {
    return fail(
      'managerApplyStatus.api_version',
      'manyforge.manager_command_response.v1',
    );
  }
  if (!booleanAt(record.ok, 'managerApplyStatus.ok')) {
    return fail('managerApplyStatus.ok', 'true for an observed operation');
  }
  if (record.error !== null) {
    return fail('managerApplyStatus.error', 'null for an observed operation');
  }
  const phase = managerApplyLifecyclePhaseAt(
    record.phase,
    'managerApplyStatus.phase',
  );
  const progress = objectAt(
    record.last_progress,
    'managerApplyStatus.last_progress',
  );
  exactKeysAt(
    progress,
    ['phase', 'record_digest'],
    'managerApplyStatus.last_progress',
  );
  const progressPhase = managerApplyLifecyclePhaseAt(
    progress.phase,
    'managerApplyStatus.last_progress.phase',
  );
  if (progressPhase !== phase) {
    return fail(
      'managerApplyStatus.last_progress.phase',
      'the reported operation phase',
    );
  }
  const activation = objectAt(
    record.prepared_activation,
    'managerApplyStatus.prepared_activation',
  );
  exactKeysAt(activation, [
    'captured_input_digest', 'deployment_artifact_id', 'deployment_revision',
    'program_binding_id', 'prepared_activation_id',
  ], 'managerApplyStatus.prepared_activation');
  const state = stringAt(record.state, 'managerApplyStatus.state');
  if (!['pending', 'succeeded', 'failed', 'unknown_effect'].includes(state)) {
    return fail(
      'managerApplyStatus.state',
      'pending, succeeded, failed, or unknown_effect',
    );
  }
  const candidate = record.candidate_projection_revision === null
    ? null
    : parseProjectionRevision(
      record.candidate_projection_revision,
      'managerApplyStatus.candidate_projection_revision',
    );
  const failure = record.failure === null
    ? null
    : (() => {
      const failureRecord = objectAt(record.failure, 'managerApplyStatus.failure');
      exactKeysAt(failureRecord, [
        'code', 'effect_disposition', 'message', 'phase',
      ], 'managerApplyStatus.failure');
      const failurePhase = managerApplyLifecyclePhaseAt(
        failureRecord.phase,
        'managerApplyStatus.failure.phase',
      );
      const effectDisposition = stringAt(
        failureRecord.effect_disposition,
        'managerApplyStatus.failure.effect_disposition',
      );
      if (effectDisposition !== 'known' && effectDisposition !== 'unknown') {
        return fail(
          'managerApplyStatus.failure.effect_disposition',
          'known or unknown',
        );
      }
      return {
        code: boundedText(
          failureRecord.code,
          'managerApplyStatus.failure.code',
          128,
        ),
        message: boundedText(
          failureRecord.message,
          'managerApplyStatus.failure.message',
          1024,
        ),
        phase: failurePhase,
        effectDisposition: effectDisposition as ManagerApplyFailure['effectDisposition'],
      };
    })();
  if ((state === 'succeeded') !== (candidate !== null)) {
    return fail(
      'managerApplyStatus.candidate_projection_revision',
      'present only for a terminal success',
    );
  }
  const failed = state === 'failed' || state === 'unknown_effect';
  if (failed !== (failure !== null)) {
    return fail(
      'managerApplyStatus.failure',
      'present only for a terminal failure',
    );
  }
  if (state === 'succeeded' && phase !== 'settled') {
    return fail('managerApplyStatus.phase', 'settled for terminal success');
  }
  if (failure !== null) {
    if (failure.phase !== phase) {
      return fail(
        'managerApplyStatus.failure.phase',
        'the reported operation phase',
      );
    }
    const expected = state === 'failed' ? 'known' : 'unknown';
    if (failure.effectDisposition !== expected) {
      return fail(
        'managerApplyStatus.failure.effect_disposition',
        expected,
      );
    }
  }
  return {
    apiVersion,
    applyGeneration: positiveIntegerAt(
      record.apply_generation,
      'managerApplyStatus.apply_generation',
    ),
    applyId: stringAt(record.apply_id, 'managerApplyStatus.apply_id'),
    sceneInitialization: parseSceneInitialization(record.scene_initialization, 'managerApplyStatus.scene_initialization'),
    candidateProjectionRevision: candidate,
    error: null,
    failure,
    lastProgress: {
      phase: progressPhase,
      recordDigest: digestAt(
        progress.record_digest,
        'managerApplyStatus.last_progress.record_digest',
      ),
    },
    ok: true,
    phase,
    preparedActivation: {
      capturedInputDigest: digestAt(
        activation.captured_input_digest,
        'managerApplyStatus.prepared_activation.captured_input_digest',
      ),
      deploymentArtifactId: stringAt(
        activation.deployment_artifact_id,
        'managerApplyStatus.prepared_activation.deployment_artifact_id',
      ),
      deploymentRevision: digestAt(
        activation.deployment_revision,
        'managerApplyStatus.prepared_activation.deployment_revision',
      ),
      programBindingId: activation.program_binding_id === null
        ? null
        : digestAt(
          activation.program_binding_id,
          'managerApplyStatus.prepared_activation.program_binding_id',
        ),
      preparedActivationId: digestAt(
        activation.prepared_activation_id,
        'managerApplyStatus.prepared_activation.prepared_activation_id',
      ),
    },
    requestId: stringAt(record.request_id, 'managerApplyStatus.request_id'),
    requestOwnerEpoch: stringAt(
      record.request_owner_epoch,
      'managerApplyStatus.request_owner_epoch',
    ),
    state: state as ManagerApplyStatus['state'],
  };
}

export function parsePreparedActivationOrigin(value: unknown, path: string): PreparedActivationOrigin {
  const row = objectAt(value, path);
  if (row.kind === 'scenario') {
    exactKeysAt(row, ['kind', 'scenario_id', 'source_document_digest'], path);
    return { kind: 'scenario', scenarioId: stringAt(row.scenario_id, `${path}.scenario_id`),
      sourceDocumentDigest: digestAt(row.source_document_digest, `${path}.source_document_digest`) };
  }
  if (row.kind !== 'sealed_canonical_empty') return fail(`${path}.kind`, 'scenario or sealed_canonical_empty');
  exactKeysAt(row, ['kind', 'cell_id', 'deployment_artifact_id', 'deployment_revision',
    'retained_bundle_id', 'graph_revision', 'admission_digest'], path);
  const identity = (value: unknown, name: string): string => {
    const text = stringAt(value, `${path}.${name}`);
    if (!text || text.normalize('NFC') !== text || new TextEncoder().encode(text).length > 128) {
      return fail(`${path}.${name}`, 'nonempty NFC identity of at most 128 UTF-8 bytes');
    }
    return text;
  };
  return { kind: 'sealed_canonical_empty', cellId: identity(row.cell_id, 'cell_id'),
    deploymentArtifactId: identity(row.deployment_artifact_id, 'deployment_artifact_id'),
    deploymentRevision: digestAt(row.deployment_revision, `${path}.deployment_revision`),
    retainedBundleId: digestAt(row.retained_bundle_id, `${path}.retained_bundle_id`),
    graphRevision: digestAt(row.graph_revision, `${path}.graph_revision`),
    admissionDigest: digestAt(row.admission_digest, `${path}.admission_digest`) };
}

export function parsePreparedActivationInput(
  value: unknown,
  path: string,
): PreparedActivationInput {
  const record = objectAt(value, path);
  exactKeysAt(record, [
    'schema_version', 'prepared_activation_id', 'origin',
    'captured_input_digest', 'deployment_artifact_id',
    'deployment_revision', 'retained_bundle_id', 'graph_revision',
    'program_id', 'program_revision', 'program_outcome',
    'node_environment_digest', 'assistant_mode',
  ], path);
  const schemaVersion = stringAt(record.schema_version, `${path}.schema_version`);
  if (schemaVersion !== 'manyforge.prepared_activation_input.v6') {
    return fail(
      `${path}.schema_version`,
      'manyforge.prepared_activation_input.v6',
    );
  }
  const outcome = objectAt(record.program_outcome, `${path}.program_outcome`);
  const origin = parsePreparedActivationOrigin(record.origin, `${path}.origin`);
  const outcomeKind = stringAt(outcome.kind, `${path}.program_outcome.kind`);
  let programBindingId: string | null;
  if (outcomeKind === 'no_program') {
    exactKeysAt(outcome, ['kind'], `${path}.program_outcome`);
    if (record.program_id !== null || record.program_revision !== null
      || record.assistant_mode !== 'optional') {
      return fail(path, 'no_program with no Program identities');
    }
    programBindingId = null;
  } else if (outcomeKind === 'bound') {
    exactKeysAt(outcome, ['kind', 'program_binding_id'], `${path}.program_outcome`);
    programBindingId = digestAt(
      outcome.program_binding_id,
      `${path}.program_outcome.program_binding_id`,
    );
  } else if (outcomeKind === 'non_executable') {
    exactKeysAt(outcome, ['kind', 'outcome'], `${path}.program_outcome`);
    objectAt(outcome.outcome, `${path}.program_outcome.outcome`);
    programBindingId = null;
  } else {
    return fail(`${path}.program_outcome.kind`, 'bound, non_executable, or no_program');
  }
  if (origin.kind === 'sealed_canonical_empty' && (outcomeKind !== 'no_program'
    || origin.deploymentArtifactId !== record.deployment_artifact_id
    || origin.deploymentRevision !== record.deployment_revision
    || origin.retainedBundleId !== record.retained_bundle_id
    || origin.graphRevision !== record.graph_revision)) {
    return fail(path, 'sealed empty origin matching its exact no-Program candidate');
  }
  return {
    schemaVersion,
    preparedActivationId: digestAt(record.prepared_activation_id,
      `${path}.prepared_activation_id`),
    origin,
    capturedInputDigest: digestAt(record.captured_input_digest,
      `${path}.captured_input_digest`),
    deploymentArtifactId: stringAt(
      record.deployment_artifact_id,
      `${path}.deployment_artifact_id`,
    ),
    deploymentRevision: digestAt(
      record.deployment_revision,
      `${path}.deployment_revision`,
    ),
    retainedBundleId: digestAt(
      record.retained_bundle_id,
      `${path}.retained_bundle_id`,
    ),
    graphRevision: digestAt(record.graph_revision, `${path}.graph_revision`),
    programId: outcomeKind === 'no_program' ? null : stringAt(record.program_id, `${path}.program_id`),
    programRevision: outcomeKind === 'no_program' ? null : digestAt(
      record.program_revision,
      `${path}.program_revision`,
    ),
    programBindingId,
    nodeEnvironmentDigest: digestAt(record.node_environment_digest,
      `${path}.node_environment_digest`),
    assistantMode: (() => {
      const mode = stringAt(record.assistant_mode, `${path}.assistant_mode`);
      if (mode !== 'required' && mode !== 'optional' && mode !== 'forbidden') {
        return fail(`${path}.assistant_mode`, 'required, optional, or forbidden');
      }
      return mode;
    })(),
  };
}
