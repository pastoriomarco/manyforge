import { parseNanoseconds, type Nanoseconds } from '../types/nanoseconds';
import type {
  RetainedNeutralProjection,
  CatalogBlackboardBinding,
  CatalogCapabilityRequirement,
  CatalogEntry,
  CatalogParameter,
  CatalogResponse,
  CurrentScenarioResponse,
  DraftMutationResponse,
  DeploymentCandidateCompilation,
  DeploymentCandidateOperationResponse,
  DeploymentWorkspaceResponse,
  ProgramDraftProjection,
  ProgramDraftApplicability,
  ProgramDraftMeshAssetMetadata,
  FileBrowserEntry,
  FileBrowserResponse,
  HealthResponse,
  JsonValue,
  NodeEnvironmentResponse,
  NodeImplementationAvailability,
  AppliedProgramSelection,
  ManagerActiveProjection,
  ManagerApplyAvailability,
  ManagerApplyPhase,
  ManagerApplyRecord,
  ManagerApplyReplayStatus,
  ManagerApplyResponse,
  ManagerDeploymentCandidatePreviewResponse,
  ManagerDeploymentRecordDiff,
  ManagerDiffProjection,
  ManagerGraphProjection,
  ManagerRuntimeProjection,
  ManagerSourceMetadata,
  ManagerStatusProjection,
  ProgramDocument,
  ProgramAuthoringDocument,
  ProgramInterventionDelegate,
  ProgramModificationPolicyValue,
  ProgramNodeModificationPolicy,
  ProgramNodeRecoveryPolicy,
  ProgramParameterDeclaration,
  ProgramOperatorStopPolicy,
  ProgramPersistenceCeiling,
  ProgramRecoveryAction,
  ProgramSceneFrameRef,
  ProgramScenePose,
  ProgramSceneResource,
  ProgramSceneShape,
  ProgramTreeNode,
  ProgramCycleStatus,
  ProgramRunStateName,
  ProgramRunStatus,
  ProgramExecutionSelectionControls,
  CycleRecoveryAction,
  CycleRecoveryActionId,
  CycleRecoveryOffer,
  ProgramRuntimeControlProjection,
  ProgramRuntimeControlsSnapshot,
  RuntimeControlAuditEvent,
  RuntimeOverrideObservationCandidate,
  RuntimeOverridePromotionProposal,
  RuntimeOverrideRecord,
  RuntimeOverrideStatus,
  RuntimeOverrideMaterializationResponse,
  ProgramValueSource,
  ProgramViewerMetadata,
  ProgramWorkspaceResponse,
  ProgramWorkspaceTransitionResponse,
  ScenarioCatalogResponse,
  CarriedObjectAttachment,
  LogicalSceneObject,
  LogicalScenePlacement,
  LogicalScenePose,
  LogicalSceneProjection,
  LogicalSceneShape,
  ProjectionRevision,
  QuaternionXyzwTuple,
  SceneRuntimeProjection,
  SceneRuntimeGoodObservationPayload,
  SceneRuntimeObservationPose,
  SceneRuntimePoseSource,
  SceneRuntimeUnknownObservationPayload,
  SceneMeshAssetMetadata,
  SceneTransactionResponse,
  SceneUrdfCollision,
  SceneUrdfJoint,
  SceneUrdfLink,
  SceneUrdfManifest,
  SceneUrdfOrigin,
  SceneUrdfPrimitive,
  SceneUrdfVisual,
  SourceModelCoordinate,
  SourceModelOccurrence,
  SourceModelProjection,
  SourceModelSrdfGroup,
  SourceModelTarget,
  Vector3Tuple,
} from '../types/api';

export function parseProgramCycleStatus(value: unknown): ProgramCycleStatus {
  const record = objectAt(value, 'programCycleStatus');
  exactKeysAt(record, [
    'program_runner_owner_epoch', 'cycle_generation', 'state', 'program_run_id',
    'cycle_claim_generation', 'terminal_or_settlement_reference',
    'run_revision', 'node_status_revision',
    'runtime_control_revision', 'recovery_offer', 'run', 'runtime_controls',
    'execution_control_revision', 'causal_correlation_id',
    'causal_session_id', 'causal_client_sequence', 'current_command_kind',
    'failure', 'reset_phase', 'allowed_commands',
  ], 'programCycleStatus');
  const state = stringAt(record.state, 'programCycleStatus.state');
  if (!['idle_after_reset', 'starting', 'running', 'stopping',
    'resetting', 'terminal_awaiting_reset', 'recovery_required'].includes(state)) {
    return fail('programCycleStatus.state', 'a supported Program cycle state');
  }
  const parsed: ProgramCycleStatus = {
    programRunnerOwnerEpoch: stringAt(
      record.program_runner_owner_epoch,
      'programCycleStatus.program_runner_owner_epoch',
    ),
    cycleGeneration: positiveIntegerAt(
      record.cycle_generation,
      'programCycleStatus.cycle_generation',
    ),
    state: state as ProgramCycleStatus['state'],
    programRunId: nullableStringAt(
      record.program_run_id,
      'programCycleStatus.program_run_id',
    ),
    cycleClaimGeneration: record.cycle_claim_generation === null
      ? null
      : positiveIntegerAt(
        record.cycle_claim_generation,
        'programCycleStatus.cycle_claim_generation',
      ),
    terminalOrSettlementReference:
      record.terminal_or_settlement_reference === null
        ? null
        : digestAt(
          record.terminal_or_settlement_reference,
          'programCycleStatus.terminal_or_settlement_reference',
        ),
    runRevision: record.run_revision === null
      ? null
      : positiveIntegerAt(record.run_revision, 'programCycleStatus.run_revision'),
    nodeStatusRevision: record.node_status_revision === null
      ? null
      : positiveIntegerAt(
        record.node_status_revision,
        'programCycleStatus.node_status_revision',
      ),
    runtimeControlRevision: record.runtime_control_revision === null
      ? null
      : positiveIntegerAt(
        record.runtime_control_revision,
        'programCycleStatus.runtime_control_revision',
      ),
    executionControlRevision: positiveIntegerAt(
      record.execution_control_revision,
      'programCycleStatus.execution_control_revision',
    ),
    causalCorrelationId: nullableStringAt(
      record.causal_correlation_id,
      'programCycleStatus.causal_correlation_id',
    ),
    causalSessionId: nullableStringAt(
      record.causal_session_id,
      'programCycleStatus.causal_session_id',
    ),
    causalClientSequence: nullablePositiveUint64TextAt(
      record.causal_client_sequence,
      'programCycleStatus.causal_client_sequence',
    ),
    currentCommandKind: nullableStringAt(
      record.current_command_kind,
      'programCycleStatus.current_command_kind',
    ),
    failure: record.failure === null
      ? null
      : jsonRecordAt(record.failure, 'programCycleStatus.failure'),
    resetPhase: nullableStringAt(
      record.reset_phase,
      'programCycleStatus.reset_phase',
    ),
    allowedCommands: arrayAt(
      record.allowed_commands,
      'programCycleStatus.allowed_commands',
    ).map((value, index) => {
      const path = `programCycleStatus.allowed_commands[${index}]`;
      const command = objectAt(value, path);
      exactKeysAt(command, [
        'command_kind', 'semantic_target', 'allowed', 'disabled_reason',
      ], path);
      return {
        commandKind: stringAt(command.command_kind, `${path}.command_kind`),
        semanticTarget: jsonRecordAt(command.semantic_target, `${path}.semantic_target`),
        allowed: booleanAt(command.allowed, `${path}.allowed`),
        disabledReason: nullableStringAt(
          command.disabled_reason,
          `${path}.disabled_reason`,
        ),
      };
    }),
    recoveryOffer: record.recovery_offer === null
      ? null
      : parseCycleRecoveryOffer(
        record.recovery_offer,
        'programCycleStatus.recovery_offer',
      ),
    run: record.run === null ? null : parseProgramRunStatus(record.run),
    runtimeControls: record.runtime_controls === null
      ? null
      : parseProgramRuntimeControlProjection(record.runtime_controls),
  };
  if (parsed.run === null) {
    if (parsed.runRevision !== null || parsed.nodeStatusRevision !== null) {
      return fail('programCycleStatus.run', 'the advertised run revisions');
    }
  } else if (
    parsed.run.capture.programRunId !== parsed.programRunId
    || (
      !['terminal_awaiting_reset', 'resetting'].includes(parsed.state)
      && parsed.run.ownerEpoch !== parsed.programRunnerOwnerEpoch
    )
    || parsed.run.runRevision !== parsed.runRevision
    || parsed.run.nodeStatusRevision !== parsed.nodeStatusRevision
  ) {
    return fail('programCycleStatus.run', 'the exact advertised run projection');
  }
  if (parsed.runtimeControls === null) {
    if (parsed.runtimeControlRevision !== null) {
      return fail(
        'programCycleStatus.runtimeControls',
        'the advertised runtime-control revision',
      );
    }
  } else if (
    parsed.run === null
    || parsed.runtimeControls.programRunId !== parsed.programRunId
    || parsed.runtimeControls.ownerEpoch !== parsed.programRunnerOwnerEpoch
    || parsed.runtimeControls.runRevision !== parsed.runRevision
    || parsed.runtimeControls.runtimeControls.runtimeControlRevision
      !== parsed.runtimeControlRevision
  ) {
    return fail(
      'programCycleStatus.runtimeControls',
      'the exact advertised runtime-control projection',
    );
  }
  return parsed;
}
import type {
  AlarmLifecycleState,
  AlarmMembership,
  AlarmPriority,
  AlarmSubject,
  ComponentOverviewProjection,
  ComposerHeadlessState,
  HeadlessRuntimeSnapshot,
  HeadlessSourceSection,
  HistorianEvent,
  HistorianEventSummary,
  HistorianDetail,
  HistorianPage,
  HistorianDiscovery,
  HistorianRetention,
  OperatorAlarmRecord,
  OperatorAlarmSnapshot,
  SourceFreshness,
} from '../types/headless';
import type {
  CapabilityCommandPreviewReceipt,
  CoordinateKind,
  DirectControlJoint,
  ManualControlContracts,
  ManualControlGripperDescriptor,
  ManualControlMotionDescriptor,
  ExecutionIdentity,
  ExecutionOwnerStatus,
  MotionPreviewReceipt,
  ObservedGripperTarget,
  ObservedMotionTarget,
  OperationObservation,
  RuntimeScaleControlAccepted,
  RuntimeScaleControlStatus,
  OperationStateName,
  ResourceSourceKind,
  SceneRevisionValue,
  ManualControlAvailability,
  ManualProgramHandoverContext,
} from '../types/robotControl';
import {
  parseProgramBindingTransport,
  ProgramBindingTransportError,
} from './programBindingTransport.generated';

type JsonObject = Record<string, unknown>;

export class StructuralContractError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'StructuralContractError';
  }
}

function fail(path: string, expected: string): never {
  throw new StructuralContractError(`${path} must be ${expected}`);
}

function objectAt(value: unknown, path: string): JsonObject {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) {
    return fail(path, 'an object');
  }
  return value as JsonObject;
}

function exactKeysAt(
  value: JsonObject,
  expected: readonly string[],
  path: string,
): void {
  const actual = Object.keys(value).sort();
  const required = [...expected].sort();
  if (
    actual.length !== required.length
    || actual.some((key, index) => key !== required[index])
  ) {
    fail(path, `an object with exactly [${required.join(', ')}]`);
  }
}

function allowedKeysAt(
  value: JsonObject,
  required: readonly string[],
  optional: readonly string[],
  path: string,
): void {
  const actual = new Set(Object.keys(value));
  const allowed = new Set([...required, ...optional]);
  if (
    required.some((key) => !actual.has(key))
    || [...actual].some((key) => !allowed.has(key))
  ) {
    fail(
      path,
      `an object with required [${required.join(', ')}] and optional [${optional.join(', ')}] fields`,
    );
  }
}

function arrayAt(value: unknown, path: string): unknown[] {
  if (!Array.isArray(value)) {
    return fail(path, 'an array');
  }
  return value;
}

function stringAt(value: unknown, path: string): string {
  if (typeof value !== 'string') {
    return fail(path, 'a string');
  }
  return value;
}

function runtimeScalePairIdentity(componentRoleId: string, targetRoleId: string): string {
  return JSON.stringify([componentRoleId, targetRoleId]);
}

function nonnegativeInt64TextAt(value: unknown, path: string): string {
  const text = stringAt(value, path);
  if (
    text.length > 19
    || !/^(0|[1-9]\d*)$/.test(text)
    || BigInt(text) > 9_223_372_036_854_775_807n
  ) {
    return fail(path, 'a canonical nonnegative int64 decimal string');
  }
  return text;
}

function positiveUint64TextAt(value: unknown, path: string): string {
  const text = stringAt(value, path);
  if (
    text.length > 20
    || !/^[1-9]\d*$/.test(text)
    || BigInt(text) > 18_446_744_073_709_551_615n
  ) {
    return fail(path, 'a canonical positive uint64 decimal string');
  }
  return text;
}

function nullablePositiveUint64TextAt(
  value: unknown,
  path: string,
): string | null {
  return value === null ? null : positiveUint64TextAt(value, path);
}

function digestAt(value: unknown, path: string): string {
  const digest = stringAt(value, path);
  if (!/^sha256:[0-9a-f]{64}$/.test(digest)) {
    return fail(path, 'an exact SHA-256 identity');
  }
  return digest;
}

function nullableStringAt(value: unknown, path: string): string | null {
  if (value === null) {
    return null;
  }
  return stringAt(value, path);
}

function booleanAt(value: unknown, path: string): boolean {
  if (typeof value !== 'boolean') {
    return fail(path, 'a boolean');
  }
  return value;
}

function numberAt(value: unknown, path: string): number {
  if (typeof value !== 'number' || !Number.isFinite(value)) {
    return fail(path, 'a finite number');
  }
  return value;
}

function integerAt(value: unknown, path: string): number {
  const number = numberAt(value, path);
  if (!Number.isSafeInteger(number) || number < 0) {
    return fail(path, 'a non-negative safe integer');
  }
  return number;
}

function positiveIntegerAt(value: unknown, path: string): number {
  const integer = integerAt(value, path);
  if (integer === 0) return fail(path, 'a positive safe integer');
  return integer;
}

function positiveNumberAt(value: unknown, path: string): number {
  const number = numberAt(value, path);
  if (number <= 0) return fail(path, 'a positive finite number');
  return number;
}

function monotonicNanosecondsAt(value: unknown, path: string): Nanoseconds {
  return parseNanoseconds(value, path);
}

function nullableMonotonicNanosecondsAt(
  value: unknown,
  path: string,
): Nanoseconds | null {
  return value === null ? null : monotonicNanosecondsAt(value, path);
}

function boundedArrayAt(
  value: unknown,
  path: string,
  maximum: number,
): unknown[] {
  const array = arrayAt(value, path);
  if (array.length > maximum) {
    return fail(path, `an array with at most ${maximum} entries`);
  }
  return array;
}

function jsonValueAt(value: unknown, path: string, depth = 0): JsonValue {
  if (depth > 32) return fail(path, 'a JSON value within the Program depth bound');
  if (value === null || typeof value === 'string' || typeof value === 'boolean') {
    return value;
  }
  if (typeof value === 'number') {
    if (!Number.isFinite(value) || (Number.isInteger(value) && !Number.isSafeInteger(value))) {
      return fail(path, 'a finite I-JSON number');
    }
    return value;
  }
  if (Array.isArray(value)) {
    return value.map((item, index) => jsonValueAt(item, `${path}[${index}]`, depth + 1));
  }
  const record = objectAt(value, path);
  return Object.fromEntries(
    Object.entries(record).map(([key, item]) => [
      key,
      jsonValueAt(item, `${path}.${key}`, depth + 1),
    ]),
  );
}

function jsonRecordAt(value: unknown, path: string): Record<string, JsonValue> {
  const parsed = jsonValueAt(value, path);
  if (parsed === null || typeof parsed !== 'object' || Array.isArray(parsed)) {
    return fail(path, 'a JSON object');
  }
  return parsed;
}

function programLiteralAt(value: unknown, path: string): JsonValue {
  const parsed = jsonValueAt(value, path);
  const inspect = (item: JsonValue, itemPath: string): void => {
    if (typeof item === 'string' && /^\$\{param:[^}]+\}$/.test(item)) {
      fail(itemPath, 'a literal value or an explicit value_sources binding');
    }
    if (Array.isArray(item)) {
      item.forEach((nested, index) => inspect(nested, `${itemPath}[${index}]`));
    } else if (item !== null && typeof item === 'object') {
      Object.entries(item).forEach(([key, nested]) => inspect(nested, `${itemPath}.${key}`));
    }
  };
  inspect(parsed, path);
  return parsed;
}

function programLiteralRecordAt(
  value: unknown,
  path: string,
): Record<string, JsonValue> {
  const record = objectAt(value, path);
  return Object.fromEntries(
    Object.entries(record).map(([key, item]) => [
      key,
      programLiteralAt(item, `${path}.${key}`),
    ]),
  );
}

function nullableNumberAt(value: unknown, path: string): number | null {
  if (value === null) {
    return null;
  }
  return numberAt(value, path);
}

function stringArrayAt(value: unknown, path: string): string[] {
  return arrayAt(value, path).map((item, index) =>
    stringAt(item, `${path}[${index}]`),
  );
}

function canonicalStringArrayAt(value: unknown, path: string): string[] {
  const strings = stringArrayAt(value, path);
  const canonical = [...new Set(strings)].sort();
  if (
    canonical.length !== strings.length
    || canonical.some((item, index) => item !== strings[index])
  ) {
    return fail(path, 'a sorted array of unique strings');
  }
  return strings;
}

function vector3At(value: unknown, path: string): Vector3Tuple {
  const array = arrayAt(value, path);
  if (array.length !== 3) {
    return fail(path, 'a three-number tuple');
  }
  return [
    numberAt(array[0], `${path}[0]`),
    numberAt(array[1], `${path}[1]`),
    numberAt(array[2], `${path}[2]`),
  ];
}

function quaternionAt(value: unknown, path: string): QuaternionXyzwTuple {
  const array = arrayAt(value, path);
  if (array.length !== 4) {
    return fail(path, 'a four-number xyzw quaternion');
  }
  const quaternion: QuaternionXyzwTuple = [
    numberAt(array[0], `${path}[0]`),
    numberAt(array[1], `${path}[1]`),
    numberAt(array[2], `${path}[2]`),
    numberAt(array[3], `${path}[3]`),
  ];
  const norm = Math.hypot(...quaternion);
  if (norm < 1e-12) {
    return fail(path, 'a non-zero xyzw quaternion');
  }
  return quaternion;
}

function stringRecordAt(value: unknown, path: string): Record<string, string> {
  const record = objectAt(value, path);
  return Object.fromEntries(
    Object.entries(record).map(([key, entry]) => [
      key,
      stringAt(entry, `${path}.${key}`),
    ]),
  );
}

function parseParameter(value: unknown, path: string): CatalogParameter {
  const record = objectAt(value, path);
  const hasDefault = booleanAt(record.hasDefault, `${path}.hasDefault`);
  const parameter: CatalogParameter = {
    id: stringAt(record.id, `${path}.id`),
    required: booleanAt(record.required, `${path}.required`),
    // The v2 catalog carries a hashed JSON Schema document. This structural
    // client requires an object boundary here; the schema helper validates the
    // supported authoring keywords when a value is checked.
    schema: objectAt(
      record.schema,
      `${path}.schema`,
    ),
    schemaHash: stringAt(record.schemaHash, `${path}.schemaHash`),
    hasDefault,
  };
  if (hasDefault) {
    parameter.default = record.default;
  }
  return parameter;
}

function parseCapability(
  value: unknown,
  path: string,
): CatalogCapabilityRequirement {
  const record = objectAt(value, path);
  const requirement: CatalogCapabilityRequirement = {
    capabilityId: stringAt(record.capabilityId, `${path}.capabilityId`),
    scope: stringAt(record.scope, `${path}.scope`),
  };
  for (const key of [
    'targetParameter',
    'componentParameter',
    'whenParameter',
  ] as const) {
    if (record[key] !== undefined) {
      requirement[key] = stringAt(record[key], `${path}.${key}`);
    }
  }
  if (record.whenEquals !== undefined) {
    requirement.whenEquals = record.whenEquals;
  }
  return requirement;
}

function parseBlackboardBinding(
  value: unknown,
  path: string,
): CatalogBlackboardBinding {
  const record = objectAt(value, path);
  const binding: CatalogBlackboardBinding = {
    keyParameter: stringAt(record.keyParameter, `${path}.keyParameter`),
    valueType: stringAt(record.valueType, `${path}.valueType`),
  };
  if (record.missingKey !== undefined) {
    binding.missingKey = stringAt(record.missingKey, `${path}.missingKey`);
  }
  return binding;
}

function parseCatalogEntry(value: unknown, path: string): CatalogEntry {
  const record = objectAt(value, path);
  const execution = objectAt(record.execution, `${path}.execution`);
  const blackboard = objectAt(record.blackboard, `${path}.blackboard`);
  const rawOwnerContext = record.ownerContext;
  const ownerContext = rawOwnerContext === null
    ? null
    : objectAt(rawOwnerContext, `${path}.ownerContext`);

  return {
    id: stringAt(record.id, `${path}.id`),
    name: stringAt(record.name, `${path}.name`),
    category: stringAt(record.category, `${path}.category`),
    description: stringAt(record.description, `${path}.description`),
    stability: stringAt(record.stability, `${path}.stability`),
    kind: stringAt(record.kind, `${path}.kind`),
    childrenMin: numberAt(record.childrenMin, `${path}.childrenMin`),
    childrenMax: numberAt(record.childrenMax, `${path}.childrenMax`),
    parameters: arrayAt(record.parameters, `${path}.parameters`).map(
      (item, index) => parseParameter(item, `${path}.parameters[${index}]`),
    ),
    requiredCapabilities: arrayAt(
      record.requiredCapabilities,
      `${path}.requiredCapabilities`,
    ).map((item, index) =>
      parseCapability(item, `${path}.requiredCapabilities[${index}]`),
    ),
    execution: {
      blocking: booleanAt(execution.blocking, `${path}.execution.blocking`),
      cancellable: booleanAt(
        execution.cancellable,
        `${path}.execution.cancellable`,
      ),
      ownerApi: stringAt(execution.ownerApi, `${path}.execution.ownerApi`),
    },
    runners: stringRecordAt(record.runners, `${path}.runners`),
    tags: stringArrayAt(record.tags, `${path}.tags`),
    minProgramProfile: nullableStringAt(
      record.minProgramProfile,
      `${path}.minProgramProfile`,
    ),
    blackboard: {
      reads: arrayAt(blackboard.reads, `${path}.blackboard.reads`).map(
        (item, index) =>
          parseBlackboardBinding(item, `${path}.blackboard.reads[${index}]`),
      ),
      writes: arrayAt(blackboard.writes, `${path}.blackboard.writes`).map(
        (item, index) =>
          parseBlackboardBinding(item, `${path}.blackboard.writes[${index}]`),
      ),
    },
    ownerContext: ownerContext === null
      ? null
      : {
          kind: stringAt(ownerContext.kind, `${path}.ownerContext.kind`),
          required: booleanAt(
            ownerContext.required,
            `${path}.ownerContext.required`,
          ),
        },
    outcomes: objectAt(record.outcomes, `${path}.outcomes`),
    eventFamily: nullableStringAt(record.eventFamily, `${path}.eventFamily`),
  };
}

export function parseHealthResponse(value: unknown): HealthResponse {
  const record = objectAt(value, 'health');
  const mode = stringAt(record.mode, 'health.mode');
  if (mode !== 'structural_only' && mode !== 'headless_client') {
    return fail('health.mode', '"structural_only" or "headless_client"');
  }
  const availability = record.runtimeAvailability === null
    ? null
    : objectAt(record.runtimeAvailability, 'health.runtimeAvailability');
  if (mode === 'structural_only' && availability === null) {
    return fail('health.runtimeAvailability', 'an unavailable-runtime object');
  }
  return {
    status: stringAt(record.status, 'health.status'),
    mode,
    components: stringRecordAt(record.components, 'health.components'),
    runtimeAvailability: availability === null
      ? null
      : {
          code: stringAt(availability.code, 'health.runtimeAvailability.code'),
          message: stringAt(
            availability.message,
            'health.runtimeAvailability.message',
          ),
          restorationGates: stringArrayAt(
            availability.restorationGates,
            'health.runtimeAvailability.restorationGates',
          ),
        },
    headless: record.headless === undefined
      ? null
      : objectAt(record.headless, 'health.headless'),
    timestamp: numberAt(record.timestamp, 'health.timestamp'),
  };
}

export function parseCatalogResponse(value: unknown): CatalogResponse {
  const record = objectAt(value, 'catalog');
  const apiVersion = stringAt(record.apiVersion, 'catalog.apiVersion');
  if (apiVersion !== 'manyforge.node_catalog.v2') {
    return fail(
      'catalog.apiVersion',
      'the literal "manyforge.node_catalog.v2"',
    );
  }
  return {
    apiVersion,
    contentVersion: stringAt(record.contentVersion, 'catalog.contentVersion'),
    catalogId: stringAt(record.catalogId, 'catalog.catalogId'),
    entries: arrayAt(record.entries, 'catalog.entries').map((entry, index) =>
      parseCatalogEntry(entry, `catalog.entries[${index}]`),
    ),
    catalogDigest: stringAt(record.catalogDigest, 'catalog.catalogDigest'),
    environmentDigest: stringAt(
      record.environmentDigest,
      'catalog.environmentDigest',
    ),
  };
}

function parseProgramValueSource(value: unknown, path: string): ProgramValueSource {
  const record = objectAt(value, path);
  allowedKeysAt(record, ['kind', 'reference'], ['field'], path);
  const kind = stringAt(record.kind, `${path}.kind`);
  if (
    kind !== 'program_parameter'
    && kind !== 'blackboard_key'
    && kind !== 'blackboard_field'
  ) {
    return fail(`${path}.kind`, 'a supported Program value-source kind');
  }
  const result: ProgramValueSource = {
    kind,
    reference: stringAt(record.reference, `${path}.reference`),
  };
  if (record.field !== undefined) {
    result.field = stringAt(record.field, `${path}.field`);
  }
  if ((kind === 'blackboard_field') !== (result.field !== undefined)) {
    return fail(path, 'a field only and always for blackboard_field');
  }
  return result;
}

const MODIFICATION_POLICY_VALUES = new Set<ProgramModificationPolicyValue>([
  'forbidden',
  'human_approval',
  'llm_allowed',
]);
const INTERVENTION_DELEGATES = new Set<ProgramInterventionDelegate>([
  'forbidden',
  'human_only',
  'llm_allowed',
  'program_only',
]);
const RECOVERY_ACTIONS = new Set<ProgramRecoveryAction>([
  'intervention',
  'runtime_override',
]);
const PERSISTENCE_CEILINGS = new Set<ProgramPersistenceCeiling>([
  'session_only',
  'runtime_override',
  'draft_patch',
]);
const OPERATOR_STOP_POLICIES = new Set<ProgramOperatorStopPolicy>([
  'inherit',
  'hold',
  'continue',
]);

function policyIdAt(value: unknown, path: string): string {
  const id = stringAt(value, path);
  if (!/^[a-z][a-z0-9_.-]*$/.test(id)
      || new TextEncoder().encode(id).length > 128) {
    return fail(path, 'a bounded portable lower-case policy ID');
  }
  return id;
}

function parseProgramNodeModificationPolicy(
  value: unknown,
  path: string,
): ProgramNodeModificationPolicy {
  const record = objectAt(value, path);
  exactKeysAt(
    record,
    ['runtime_override', 'persistent_edit', 'intervention_delegate'],
    path,
  );
  const runtimeOverride = stringAt(
    record.runtime_override,
    `${path}.runtime_override`,
  ) as ProgramModificationPolicyValue;
  const persistentEdit = stringAt(
    record.persistent_edit,
    `${path}.persistent_edit`,
  ) as ProgramModificationPolicyValue;
  const interventionDelegate = stringAt(
    record.intervention_delegate,
    `${path}.intervention_delegate`,
  ) as ProgramInterventionDelegate;
  if (!MODIFICATION_POLICY_VALUES.has(runtimeOverride)
      || !MODIFICATION_POLICY_VALUES.has(persistentEdit)
      || !INTERVENTION_DELEGATES.has(interventionDelegate)) {
    return fail(path, 'closed modification-policy enums');
  }
  return {
    runtime_override: runtimeOverride,
    persistent_edit: persistentEdit,
    intervention_delegate: interventionDelegate,
  };
}

function parseProgramNodeRecoveryPolicy(
  value: unknown,
  path: string,
): ProgramNodeRecoveryPolicy {
  const record = objectAt(value, path);
  exactKeysAt(
    record,
    [
      'allow_intervention',
      'allowed_recovery_actions',
      'max_retries',
      'allowed_skills',
      'persistence_ceiling',
    ],
    path,
  );
  const actions = canonicalStringArrayAt(
    boundedArrayAt(
      record.allowed_recovery_actions,
      `${path}.allowed_recovery_actions`,
      2,
    ),
    `${path}.allowed_recovery_actions`,
  ) as ProgramRecoveryAction[];
  if (actions.some((action) => !RECOVERY_ACTIONS.has(action))) {
    return fail(`${path}.allowed_recovery_actions`, 'closed recovery actions');
  }
  const maxRetries = integerAt(record.max_retries, `${path}.max_retries`);
  if (maxRetries > 255) {
    return fail(`${path}.max_retries`, 'an integer in 0..255');
  }
  const skills = boundedArrayAt(
    record.allowed_skills,
    `${path}.allowed_skills`,
    256,
  ).map((skill, index) => policyIdAt(
    skill,
    `${path}.allowed_skills[${index}]`,
  ));
  if (
    skills.some((skill, index) => (
      index > 0 && skill < skills[index - 1]
    ))
    || new Set(skills).size !== skills.length
  ) {
    return fail(`${path}.allowed_skills`, 'sorted unique policy IDs');
  }
  const allowIntervention = booleanAt(
    record.allow_intervention,
    `${path}.allow_intervention`,
  );
  const persistenceCeiling = stringAt(
    record.persistence_ceiling,
    `${path}.persistence_ceiling`,
  ) as ProgramPersistenceCeiling;
  if (!PERSISTENCE_CEILINGS.has(persistenceCeiling)) {
    return fail(`${path}.persistence_ceiling`, 'a closed persistence ceiling');
  }
  const hasIntervention = actions.includes('intervention');
  if (allowIntervention !== hasIntervention) {
    return fail(path, 'matching intervention permission and action');
  }
  if (skills.length > 0 && !hasIntervention) {
    return fail(path, 'allowed_skills only with intervention');
  }
  return {
    allow_intervention: allowIntervention,
    allowed_recovery_actions: actions,
    max_retries: maxRetries,
    allowed_skills: skills,
    persistence_ceiling: persistenceCeiling,
  };
}

function parseProgramTreeNode(
  value: unknown,
  path: string,
  depth = 0,
): ProgramTreeNode {
  if (depth > 256) return fail(path, 'a Program tree within the depth bound');
  const record = objectAt(value, path);
  allowedKeysAt(
    record,
    ['node_id', 'instance_id', 'parameters', 'children'],
    [
      'value_sources',
      'metadata',
      'modification_policy',
      'recovery_policy',
      'operator_stop_policy',
      'execution_groups',
    ],
    path,
  );
  const sources = record.value_sources === undefined
    ? undefined
    : Object.fromEntries(
        Object.entries(objectAt(record.value_sources, `${path}.value_sources`)).map(
          ([key, source]) => [
            key,
            parseProgramValueSource(source, `${path}.value_sources.${key}`),
          ],
        ),
      );
  const node: ProgramTreeNode = {
    node_id: stringAt(record.node_id, `${path}.node_id`),
    instance_id: stringAt(record.instance_id, `${path}.instance_id`),
    parameters: programLiteralRecordAt(record.parameters, `${path}.parameters`),
    children: arrayAt(record.children, `${path}.children`).map((child, index) =>
      parseProgramTreeNode(child, `${path}.children[${index}]`, depth + 1),
    ),
  };
  if (sources !== undefined) node.value_sources = sources;
  if (record.metadata !== undefined) {
    node.metadata = jsonRecordAt(record.metadata, `${path}.metadata`);
  }
  if (record.modification_policy !== undefined) {
    node.modification_policy = parseProgramNodeModificationPolicy(
      record.modification_policy,
      `${path}.modification_policy`,
    );
  }
  if (record.recovery_policy !== undefined) {
    node.recovery_policy = parseProgramNodeRecoveryPolicy(
      record.recovery_policy,
      `${path}.recovery_policy`,
    );
  }
  if (record.operator_stop_policy !== undefined) {
    const operatorStop = stringAt(
      record.operator_stop_policy,
      `${path}.operator_stop_policy`,
    ) as ProgramOperatorStopPolicy;
    if (!OPERATOR_STOP_POLICIES.has(operatorStop)) {
      return fail(`${path}.operator_stop_policy`, 'a closed Stop policy');
    }
    node.operator_stop_policy = operatorStop;
  }
  if (record.execution_groups !== undefined) {
    const groupPath = `${path}.execution_groups`;
    if (record.execution_groups === 'all') {
      node.execution_groups = 'all';
    } else {
      const groups = arrayAt(record.execution_groups, groupPath);
      if (groups.length === 0 || groups.length > 4096) {
        return fail(groupPath, 'a bounded nonempty named selection');
      }
      const names = groups.map((group, index) => {
        const name = stringAt(group, `${groupPath}[${index}]`);
        if (name.length > 128 || !/^[a-z][a-z0-9_.-]*$/.test(name)
            || name === 'all' || name === 'inherit') {
          return fail(groupPath, 'portable non-reserved execution group IDs');
        }
        return name;
      });
      if (names.some((name, index) => index > 0 && names[index - 1] >= name)) {
        return fail(groupPath, 'unique canonical-sorted execution group IDs');
      }
      node.execution_groups = names;
    }
  }
  const modification = node.modification_policy ?? {
    runtime_override: 'forbidden' as const,
    persistent_edit: 'forbidden' as const,
    intervention_delegate: 'forbidden' as const,
  };
  const recovery = node.recovery_policy ?? {
    allow_intervention: false,
    allowed_recovery_actions: [],
    max_retries: 0,
    allowed_skills: [],
    persistence_ceiling: 'session_only' as const,
  };
  const operatorStop = node.operator_stop_policy ?? 'inherit';
  if (
    node.modification_policy !== undefined
    && modification.runtime_override === 'forbidden'
    && modification.persistent_edit === 'forbidden'
    && modification.intervention_delegate === 'forbidden'
  ) {
    return fail(`${path}.modification_policy`, 'omit the canonical default policy');
  }
  if (
    node.recovery_policy !== undefined
    && !recovery.allow_intervention
    && recovery.allowed_recovery_actions.length === 0
    && recovery.max_retries === 0
    && recovery.allowed_skills.length === 0
    && recovery.persistence_ceiling === 'session_only'
  ) {
    return fail(`${path}.recovery_policy`, 'omit the canonical default policy');
  }
  if (
    node.operator_stop_policy !== undefined
    && operatorStop === 'inherit'
  ) {
    return fail(`${path}.operator_stop_policy`, 'omit the canonical default policy');
  }
  if (
    recovery.allow_intervention
    !== recovery.allowed_recovery_actions.includes('intervention')
  ) {
    return fail(path, 'matching intervention permission and action');
  }
  if (
    recovery.allowed_skills.length > 0
    && !recovery.allowed_recovery_actions.includes('intervention')
  ) {
    return fail(path, 'allowed_skills only with intervention');
  }
  if (
    recovery.allowed_recovery_actions.includes('runtime_override')
    && modification.runtime_override === 'forbidden'
  ) {
    return fail(path, 'runtime_override requires modification permission');
  }
  if (
    recovery.persistence_ceiling === 'runtime_override'
    && modification.runtime_override === 'forbidden'
  ) {
    return fail(path, 'runtime_override persistence requires modification permission');
  }
  if (
    recovery.persistence_ceiling === 'draft_patch'
    && modification.persistent_edit === 'forbidden'
  ) {
    return fail(path, 'draft_patch persistence requires persistent-edit permission');
  }
  if (
    recovery.allowed_recovery_actions.length === 0
    && (
      recovery.allow_intervention
      || recovery.max_retries !== 0
      || recovery.allowed_skills.length !== 0
      || recovery.persistence_ceiling !== 'session_only'
    )
  ) {
    return fail(path, 'non-default recovery policy requires an action');
  }
  return node;
}

function parseProgramParameter(
  value: unknown,
  path: string,
): ProgramParameterDeclaration {
  const record = objectAt(value, path);
  allowedKeysAt(
    record,
    ['parameter_id', 'type', 'default', 'runtime_mutability'],
    ['constraints', 'description'],
    path,
  );
  const runtimeMutability = stringAt(
    record.runtime_mutability,
    `${path}.runtime_mutability`,
  );
  if (
    runtimeMutability !== 'immutable'
    && runtimeMutability !== 'before_run'
    && runtimeMutability !== 'between_operations'
  ) {
    return fail(`${path}.runtime_mutability`, 'a supported mutability');
  }
  const parameter: ProgramParameterDeclaration = {
    parameter_id: stringAt(record.parameter_id, `${path}.parameter_id`),
    type: stringAt(record.type, `${path}.type`),
    default: jsonValueAt(record.default, `${path}.default`),
    runtime_mutability: runtimeMutability,
  };
  if (record.constraints !== undefined) {
    parameter.constraints = jsonRecordAt(record.constraints, `${path}.constraints`);
  }
  if (record.description !== undefined) {
    parameter.description = stringAt(record.description, `${path}.description`);
  }
  return parameter;
}

function parseProgramSceneShape(value: unknown, path: string): ProgramSceneShape {
  const record = objectAt(value, path);
  const type = stringAt(record.type, `${path}.type`);
  if (type === 'box') {
    exactKeysAt(record, ['type', 'box_dimensions_m'], path);
    const dimensions = vector3At(record.box_dimensions_m, `${path}.box_dimensions_m`);
    if (dimensions.some((item) => item <= 0)) {
      return fail(`${path}.box_dimensions_m`, 'three positive finite numbers');
    }
    return { type, box_dimensions_m: dimensions };
  }
  if (type === 'sphere') {
    exactKeysAt(record, ['type', 'sphere_radius_m'], path);
    return {
      type,
      sphere_radius_m: positiveNumberAt(
        record.sphere_radius_m,
        `${path}.sphere_radius_m`,
      ),
    };
  }
  if (type === 'cylinder') {
    exactKeysAt(
      record,
      ['type', 'cylinder_radius_m', 'cylinder_height_m'],
      path,
    );
    return {
      type,
      cylinder_radius_m: positiveNumberAt(
        record.cylinder_radius_m,
        `${path}.cylinder_radius_m`,
      ),
      cylinder_height_m: positiveNumberAt(
        record.cylinder_height_m,
        `${path}.cylinder_height_m`,
      ),
    };
  }
  if (type === 'mesh') {
    allowedKeysAt(record, ['type', 'mesh_resource_uri'], ['mesh_scale'], path);
    const shape: ProgramSceneShape = {
      type,
      mesh_resource_uri: stringAt(
        record.mesh_resource_uri,
        `${path}.mesh_resource_uri`,
      ),
    };
    if (record.mesh_scale !== undefined) {
      const scale = vector3At(record.mesh_scale, `${path}.mesh_scale`);
      if (scale.some((item) => item <= 0)) {
        return fail(`${path}.mesh_scale`, 'three positive finite numbers');
      }
      shape.mesh_scale = scale;
    }
    return shape;
  }
  return fail(`${path}.type`, 'box, sphere, cylinder, or mesh');
}

function parseProgramSceneFrameRef(
  value: unknown,
  path: string,
): ProgramSceneFrameRef {
  const record = objectAt(value, path);
  const kind = stringAt(record.kind, `${path}.kind`);
  if (kind === 'cell_frame') {
    exactKeysAt(record, ['kind'], path);
    return { kind };
  }
  if (kind === 'component_frame') {
    exactKeysAt(
      record,
      ['kind', 'component_role', 'model_role', 'frame_role'],
      path,
    );
    return {
      kind,
      component_role: stringAt(record.component_role, `${path}.component_role`),
      model_role: stringAt(record.model_role, `${path}.model_role`),
      frame_role: stringAt(record.frame_role, `${path}.frame_role`),
    };
  }
  return fail(`${path}.kind`, 'cell_frame or component_frame');
}

function parseProgramScenePose(value: unknown, path: string): ProgramScenePose {
  const record = objectAt(value, path);
  exactKeysAt(record, ['frame_ref', 'position_m', 'orientation'], path);
  const orientation = objectAt(record.orientation, `${path}.orientation`);
  const orientationKeys = Object.keys(orientation);
  if (orientationKeys.length !== 1) {
    return fail(`${path}.orientation`, 'exactly one of xyzw or rpy_deg');
  }
  let parsedOrientation: ProgramScenePose['orientation'];
  if (orientationKeys[0] === 'xyzw') {
    parsedOrientation = {
      xyzw: quaternionAt(orientation.xyzw, `${path}.orientation.xyzw`),
    };
  } else if (orientationKeys[0] === 'rpy_deg') {
    parsedOrientation = {
      rpy_deg: vector3At(orientation.rpy_deg, `${path}.orientation.rpy_deg`),
    };
  } else {
    return fail(`${path}.orientation`, 'exactly one of xyzw or rpy_deg');
  }
  return {
    frame_ref: parseProgramSceneFrameRef(record.frame_ref, `${path}.frame_ref`),
    position_m: vector3At(record.position_m, `${path}.position_m`),
    orientation: parsedOrientation,
  };
}

function parseProgramSceneResource(
  value: unknown,
  path: string,
): ProgramSceneResource {
  const record = objectAt(value, path);
  allowedKeysAt(
    record,
    ['scene_resource_id', 'shape', 'pose'],
    ['resource_roles', 'metadata', 'collision_policy_ids'],
    path,
  );
  const resource: ProgramSceneResource = {
    scene_resource_id: stringAt(
      record.scene_resource_id,
      `${path}.scene_resource_id`,
    ),
    shape: parseProgramSceneShape(record.shape, `${path}.shape`),
    pose: parseProgramScenePose(record.pose, `${path}.pose`),
  };
  if (record.resource_roles !== undefined) {
    resource.resource_roles = canonicalStringArrayAt(
      record.resource_roles,
      `${path}.resource_roles`,
    );
  }
  if (record.metadata !== undefined) {
    resource.metadata = jsonRecordAt(record.metadata, `${path}.metadata`);
  }
  if (record.collision_policy_ids !== undefined) {
    resource.collision_policy_ids = canonicalStringArrayAt(
      record.collision_policy_ids,
      `${path}.collision_policy_ids`,
    );
  }
  return resource;
}

function booleanRecordAt(value: unknown, path: string): Record<string, boolean> {
  const record = objectAt(value, path);
  const result: Record<string, boolean> = {};
  for (const key of Object.keys(record).sort()) {
    result[key] = booleanAt(record[key], `${path}.${key}`);
  }
  if (Object.keys(record).join('\u0000') !== Object.keys(record).sort().join('\u0000')) {
    return fail(path, 'a canonical-sorted visibility mapping');
  }
  return result;
}

function parseProgramViewerMetadata(
  value: unknown,
  path: string,
): ProgramViewerMetadata {
  const record = objectAt(value, path);
  allowedKeysAt(
    record,
    [],
    [
      'frame_role_visibility',
      'frame_gizmo_size_m',
      'resource_role_visibility',
      'scene_resource_axes_visibility',
      'object_axes_gizmo_size_m',
    ],
    path,
  );
  const viewer: ProgramViewerMetadata = {};
  if (record.frame_role_visibility !== undefined) {
    viewer.frame_role_visibility = booleanRecordAt(
      record.frame_role_visibility,
      `${path}.frame_role_visibility`,
    );
  }
  if (record.frame_gizmo_size_m !== undefined) {
    viewer.frame_gizmo_size_m = positiveNumberAt(
      record.frame_gizmo_size_m,
      `${path}.frame_gizmo_size_m`,
    );
  }
  if (record.resource_role_visibility !== undefined) {
    viewer.resource_role_visibility = booleanRecordAt(
      record.resource_role_visibility,
      `${path}.resource_role_visibility`,
    );
  }
  if (record.scene_resource_axes_visibility !== undefined) {
    viewer.scene_resource_axes_visibility = booleanRecordAt(
      record.scene_resource_axes_visibility,
      `${path}.scene_resource_axes_visibility`,
    );
  }
  if (record.object_axes_gizmo_size_m !== undefined) {
    viewer.object_axes_gizmo_size_m = positiveNumberAt(
      record.object_axes_gizmo_size_m,
      `${path}.object_axes_gizmo_size_m`,
    );
  }
  return viewer;
}

export function parseProgramDocument(value: unknown, path = 'program'): ProgramDocument {
  const record = objectAt(value, path);
  allowedKeysAt(
    record,
    [
      'api_version',
      'program_id',
      'program_revision',
      'program_profile',
      'name',
      'catalog',
      'component_roles',
      'target_roles',
      'tree',
    ],
    [
      'description',
      'compatibility',
      'viewer',
      'scene',
      'parameters',
      'blackboard',
      'contact_policies',
    ],
    path,
  );
  const apiVersion = stringAt(record.api_version, `${path}.api_version`);
  if (apiVersion !== 'manyforge.program.v2') {
    return fail(`${path}.api_version`, 'the literal "manyforge.program.v2"');
  }
  const programProfile = stringAt(record.program_profile, `${path}.program_profile`);
  if (programProfile !== 'composable-cell-v1') {
    return fail(`${path}.program_profile`, 'the literal "composable-cell-v1"');
  }
  const catalog = objectAt(record.catalog, `${path}.catalog`);
  exactKeysAt(catalog, ['api_version', 'content_version'], `${path}.catalog`);
  const catalogApiVersion = stringAt(catalog.api_version, `${path}.catalog.api_version`);
  if (catalogApiVersion !== 'manyforge.node_catalog.v2') {
    return fail(
      `${path}.catalog.api_version`,
      'the literal "manyforge.node_catalog.v2"',
    );
  }
  const result: ProgramDocument = {
    api_version: apiVersion,
    program_id: stringAt(record.program_id, `${path}.program_id`),
    program_revision: digestAt(record.program_revision, `${path}.program_revision`),
    program_profile: programProfile,
    name: stringAt(record.name, `${path}.name`),
    catalog: {
      api_version: catalogApiVersion,
      content_version: stringAt(catalog.content_version, `${path}.catalog.content_version`),
    },
    component_roles: jsonRecordAt(record.component_roles, `${path}.component_roles`),
    target_roles: jsonRecordAt(record.target_roles, `${path}.target_roles`),
    tree: parseProgramTreeNode(record.tree, `${path}.tree`),
  };
  if (record.description !== undefined) {
    result.description = stringAt(record.description, `${path}.description`);
  }
  if (record.compatibility !== undefined) {
    result.compatibility = jsonRecordAt(record.compatibility, `${path}.compatibility`);
  }
  if (record.viewer !== undefined) {
    result.viewer = parseProgramViewerMetadata(record.viewer, `${path}.viewer`);
  }
  if (record.scene !== undefined) {
    result.scene = boundedArrayAt(record.scene, `${path}.scene`, 4096).map(
      (item, index) => parseProgramSceneResource(item, `${path}.scene[${index}]`),
    );
    const ids = result.scene.map((resource) => resource.scene_resource_id);
    if (ids.join('\u0000') !== [...new Set(ids)].sort().join('\u0000')) {
      return fail(`${path}.scene`, 'unique resources sorted by scene_resource_id');
    }
  }
  if (record.parameters !== undefined) {
    result.parameters = arrayAt(record.parameters, `${path}.parameters`).map(
      (item, index) => parseProgramParameter(item, `${path}.parameters[${index}]`),
    );
  }
  if (record.blackboard !== undefined) {
    const blackboard = objectAt(record.blackboard, `${path}.blackboard`);
    exactKeysAt(blackboard, ['namespaces', 'keys'], `${path}.blackboard`);
    result.blackboard = {
      namespaces: arrayAt(blackboard.namespaces, `${path}.blackboard.namespaces`).map(
        (item, index) => {
          const rowPath = `${path}.blackboard.namespaces[${index}]`;
          const row = objectAt(item, rowPath);
          allowedKeysAt(row, ['namespace_id', 'owner'], ['description'], rowPath);
          const owner = stringAt(row.owner, `${rowPath}.owner`);
          if (owner !== 'program') return fail(`${rowPath}.owner`, 'the literal "program"');
          return {
            namespace_id: stringAt(row.namespace_id, `${rowPath}.namespace_id`),
            owner,
            ...(row.description === undefined
              ? {}
              : { description: stringAt(row.description, `${rowPath}.description`) }),
          };
        },
      ),
      keys: arrayAt(blackboard.keys, `${path}.blackboard.keys`).map((item, index) => {
        const rowPath = `${path}.blackboard.keys[${index}]`;
        const row = objectAt(item, rowPath);
        allowedKeysAt(
          row,
          ['key', 'type', 'access', 'persistence'],
          ['fields', 'description'],
          rowPath,
        );
        const access = stringAt(row.access, `${rowPath}.access`);
        const persistence = stringAt(row.persistence, `${rowPath}.persistence`);
        if (!['read', 'write', 'read_write'].includes(access)) {
          return fail(`${rowPath}.access`, 'a supported access mode');
        }
        if (!['node', 'run', 'persistent'].includes(persistence)) {
          return fail(`${rowPath}.persistence`, 'a supported persistence mode');
        }
        return {
          key: stringAt(row.key, `${rowPath}.key`),
          type: stringAt(row.type, `${rowPath}.type`),
          access: access as 'read' | 'write' | 'read_write',
          persistence: persistence as 'node' | 'run' | 'persistent',
          ...(row.fields === undefined
            ? {}
            : {
                fields: arrayAt(row.fields, `${rowPath}.fields`).map((field, fieldIndex) => {
                  const fieldPath = `${rowPath}.fields[${fieldIndex}]`;
                  const fieldRecord = objectAt(field, fieldPath);
                  exactKeysAt(fieldRecord, ['field', 'type'], fieldPath);
                  return {
                    field: stringAt(fieldRecord.field, `${fieldPath}.field`),
                    type: stringAt(fieldRecord.type, `${fieldPath}.type`),
                  };
                }),
              }),
          ...(row.description === undefined
            ? {}
            : { description: stringAt(row.description, `${rowPath}.description`) }),
        };
      }),
    };
  }
  if (record.contact_policies !== undefined) {
    result.contact_policies = arrayAt(
      record.contact_policies,
      `${path}.contact_policies`,
    ).map((item, index) => jsonValueAt(item, `${path}.contact_policies[${index}]`));
  }
  const declaredFrameRoles = new Set<string>();
  const declaredResourceRoles = new Set<string>();
  for (const target of Object.values(result.target_roles)) {
    if (target === null || typeof target !== 'object' || Array.isArray(target)) continue;
    for (const [field, destination] of [
      ['required_frame_roles', declaredFrameRoles],
      ['required_resource_roles', declaredResourceRoles],
    ] as const) {
      const roles = target[field];
      if (!Array.isArray(roles)) continue;
      for (const role of roles) if (typeof role === 'string') destination.add(role);
    }
  }
  for (const resource of result.scene ?? []) {
    for (const role of resource.resource_roles ?? []) declaredResourceRoles.add(role);
    if (resource.pose.frame_ref.kind !== 'component_frame') continue;
    const component = result.component_roles[resource.pose.frame_ref.component_role];
    if (component === null || typeof component !== 'object' || Array.isArray(component)) {
      return fail(
        `${path}.scene.${resource.scene_resource_id}.pose.frame_ref.component_role`,
        'a declared component role',
      );
    }
    const modelRoles = component.required_model_roles;
    if (
      Array.isArray(modelRoles)
      && !modelRoles.includes(resource.pose.frame_ref.model_role)
    ) {
      return fail(
        `${path}.scene.${resource.scene_resource_id}.pose.frame_ref.model_role`,
        'a model role required by the component role',
      );
    }
  }
  for (const [field, declared] of [
    ['frame_role_visibility', declaredFrameRoles],
    ['resource_role_visibility', declaredResourceRoles],
  ] as const) {
    for (const role of Object.keys(result.viewer?.[field] ?? {})) {
      if (!declared.has(role)) {
        return fail(`${path}.viewer.${field}.${role}`, 'a declared Program role');
      }
    }
  }
  const declaredSceneResources = new Set(
    (result.scene ?? []).map((resource) => resource.scene_resource_id),
  );
  for (const sceneResourceId of Object.keys(
    result.viewer?.scene_resource_axes_visibility ?? {},
  )) {
    if (!declaredSceneResources.has(sceneResourceId)) {
      return fail(
        `${path}.viewer.scene_resource_axes_visibility.${sceneResourceId}`,
        'a declared Program scene resource',
      );
    }
  }
  return result;
}

function parseProgramAuthoringDocument(
  value: unknown,
  path: string,
): ProgramAuthoringDocument {
  const record = objectAt(value, path);
  allowedKeysAt(
    record,
    [
      'schema_version',
      'api_version',
      'program_id',
      'program_profile',
      'name',
      'catalog',
      'component_roles',
      'target_roles',
      'forest',
      'positions',
    ],
    [
      'description',
      'compatibility',
      'viewer',
      'scene',
      'parameters',
      'blackboard',
      'contact_policies',
    ],
    path,
  );
  const schemaVersion = stringAt(record.schema_version, `${path}.schema_version`);
  if (schemaVersion !== 'manyforge.composer_authoring_forest.v1') {
    return fail(`${path}.schema_version`, 'manyforge.composer_authoring_forest.v1');
  }
  const forest = boundedArrayAt(record.forest, `${path}.forest`, 100_000).map(
    (node, index) => parseProgramTreeNode(node, `${path}.forest[${index}]`),
  );
  const syntheticRoot = forest[0] ?? {
    node_id: 'composer.placeholder',
    instance_id: 'composer_placeholder',
    parameters: {},
    children: [],
  };
  const strictShape = Object.fromEntries(
    Object.entries(record).filter(([key]) => ![
      'schema_version',
      'forest',
      'positions',
    ].includes(key)),
  );
  const parsed = parseProgramDocument(
    {
      ...strictShape,
      program_revision: `sha256:${'0'.repeat(64)}`,
      tree: syntheticRoot,
    },
    `${path}.programFields`,
  );
  const positionsRecord = objectAt(record.positions, `${path}.positions`);
  const nodeIds = new Set<string>();
  const pending = [...forest];
  while (pending.length > 0) {
    const node = pending.pop()!;
    if (nodeIds.has(node.instance_id)) {
      return fail(`${path}.forest`, 'unique node instance IDs');
    }
    nodeIds.add(node.instance_id);
    pending.push(...node.children);
  }
  const positions: Record<string, { x: number; y: number }> = {};
  for (const [instanceId, value] of Object.entries(positionsRecord)) {
    if (!nodeIds.has(instanceId)) {
      return fail(`${path}.positions.${instanceId}`, 'a retained authoring node');
    }
    const position = objectAt(value, `${path}.positions.${instanceId}`);
    exactKeysAt(position, ['x', 'y'], `${path}.positions.${instanceId}`);
    const x = numberAt(position.x, `${path}.positions.${instanceId}.x`);
    const y = numberAt(position.y, `${path}.positions.${instanceId}.y`);
    if (Math.abs(x) > 10_000_000 || Math.abs(y) > 10_000_000) {
      return fail(`${path}.positions.${instanceId}`, 'a bounded canvas position');
    }
    positions[instanceId] = { x, y };
  }
  const { program_revision, tree, ...programFields } = parsed;
  void program_revision;
  void tree;
  return {
    ...programFields,
    schema_version: schemaVersion,
    forest,
    positions,
  };
}

function parseRuntimeAvailability(value: unknown, path: string) {
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
    ['cell_graph', 'graph_revision', 'publication', 'retained_bundle_id'],
    path,
  );
  return {
    cellGraph: jsonRecordAt(record.cell_graph, path + '.cell_graph'),
    graphRevision: stringAt(record.graph_revision, path + '.graph_revision'),
    retainedBundleId: stringAt(
      record.retained_bundle_id,
      path + '.retained_bundle_id',
    ),
    publication: jsonRecordAt(record.publication, path + '.publication'),
  };
}

function parseManagerApplyAvailability(
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

const MANAGER_APPLY_PHASES = new Set<ManagerApplyPhase>([
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
      'request',
      'terminal_state',
    ],
    path,
  );
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
  if (terminalState !== null && terminalState !== 'succeeded') {
    return fail(path + '.terminal_state', 'succeeded or null');
  }
  return {
    applyGeneration: positiveIntegerAt(
      record.apply_generation,
      path + '.apply_generation',
    ),
    applyId: stringAt(record.apply_id, path + '.apply_id'),
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
    incomplete: booleanAt(record.incomplete, path + '.incomplete'),
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
    terminalState,
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
    ['api_version', 'candidate_receipt', 'converged', 'diff', 'ok'],
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
      'expected_current',
      'manager_owner_epoch',
      'receipt_digest',
      'scenario_catalog_revision',
      'schema_version',
    ],
    'managerDeploymentCandidatePreview.candidate_receipt',
  );
  const schemaVersion = stringAt(
    receipt.schema_version,
    'managerDeploymentCandidatePreview.candidate_receipt.schema_version',
  );
  if (schemaVersion !== 'manyforge.manager_deployment_candidate_receipt.v1') {
    return fail(
      'managerDeploymentCandidatePreview.candidate_receipt.schema_version',
      'manyforge.manager_deployment_candidate_receipt.v1',
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
      deployment_revision: stringAt(receipt.deployment_revision, 'candidateReceipt.deployment_revision'),
      scenario_catalog_revision: stringAt(receipt.scenario_catalog_revision, 'candidateReceipt.scenario_catalog_revision'),
      candidate_retained_bundle_id: stringAt(receipt.candidate_retained_bundle_id, 'candidateReceipt.candidate_retained_bundle_id'),
      candidate_graph_revision: stringAt(receipt.candidate_graph_revision, 'candidateReceipt.candidate_graph_revision'),
      expected_current: {
        graph_revision: stringAt(expected.graph_revision, 'candidateReceipt.expected_current.graph_revision'),
        retained_bundle_id: stringAt(expected.retained_bundle_id, 'candidateReceipt.expected_current.retained_bundle_id'),
      },
      expected_admission_generation: positiveIntegerAt(receipt.expected_admission_generation, 'candidateReceipt.expected_admission_generation'),
      manager_owner_epoch: stringAt(receipt.manager_owner_epoch, 'candidateReceipt.manager_owner_epoch'),
      receipt_digest: stringAt(receipt.receipt_digest, 'candidateReceipt.receipt_digest'),
    },
    converged: booleanAt(record.converged, 'managerDeploymentCandidatePreview.converged'),
    diff: parsedDiff,
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
  if (state !== 'pending' && state !== 'succeeded') {
    return fail('managerApply.state', 'pending or succeeded');
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
  if (state === 'pending') {
    if (record.candidate_projection_revision !== null) {
      return fail(
        'managerApply.candidate_projection_revision',
        'null while Apply is pending',
      );
    }
    return { ...base, candidateProjectionRevision: null, state };
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
    state,
  };
}

function parseProgramDraftProjection(
  value: unknown,
  path: string,
): ProgramDraftProjection {
  const draft = objectAt(value, path);
  exactKeysAt(
    draft,
    [
      'draftId',
      'draftRevision',
      'programLoaded',
      'authoring',
      'candidateAvailable',
      'programRevision',
      'program',
      'diagnostics',
    ],
    path,
  );
  const programLoaded = booleanAt(draft.programLoaded, `${path}.programLoaded`);
  const authoring = draft.authoring === null
    ? null
    : parseProgramAuthoringDocument(draft.authoring, `${path}.authoring`);
  const candidateAvailable = booleanAt(
    draft.candidateAvailable,
    `${path}.candidateAvailable`,
  );
  const program = draft.program === null
    ? null
    : parseProgramDocument(draft.program, `${path}.program`);
  const programRevision = nullableStringAt(
    draft.programRevision,
    `${path}.programRevision`,
  );
  const diagnostics = boundedArrayAt(
    draft.diagnostics,
    `${path}.diagnostics`,
    100_000,
  ).map((value, index) => {
    const diagnosticPath = `${path}.diagnostics[${index}]`;
    const record = objectAt(value, diagnosticPath);
    exactKeysAt(record, ['code', 'path', 'message', 'severity'], diagnosticPath);
    const severity = stringAt(record.severity, `${diagnosticPath}.severity`);
    if (severity !== 'error') return fail(`${diagnosticPath}.severity`, 'error');
    return {
      code: stringAt(record.code, `${diagnosticPath}.code`),
      path: stringAt(record.path, `${diagnosticPath}.path`),
      message: stringAt(record.message, `${diagnosticPath}.message`),
      severity: severity as 'error',
    };
  });
  if (
    programLoaded !== (authoring !== null)
    || candidateAvailable !== (program !== null)
    || (program?.program_revision ?? null) !== programRevision
    || (candidateAvailable && diagnostics.length !== 0)
    || (programLoaded && !candidateAvailable && diagnostics.length === 0)
  ) {
    return fail(path, 'internally consistent Program identity');
  }
  const draftId = stringAt(draft.draftId, `${path}.draftId`);
  if (!draftId) {
    return fail(`${path}.draftId`, 'a non-empty Draft identity');
  }
  return {
    draftId,
    draftRevision: integerAt(draft.draftRevision, `${path}.draftRevision`),
    programLoaded,
    authoring,
    candidateAvailable,
    programRevision,
    program,
    diagnostics,
  };
}

function parseProgramDraftApplicability(
  value: unknown,
  path: string,
): ProgramDraftApplicability {
  const record = objectAt(value, path);
  exactKeysAt(
    record,
    ['applyReady', 'affectedInstanceIds', 'unavailableReason'],
    path,
  );
  const affectedInstanceIds = boundedArrayAt(
    record.affectedInstanceIds,
    `${path}.affectedInstanceIds`,
    100_000,
  ).map((instanceId, index) => policyIdAt(
    instanceId,
    `${path}.affectedInstanceIds[${index}]`,
  ));
  if (new Set(affectedInstanceIds).size !== affectedInstanceIds.length) {
    return fail(`${path}.affectedInstanceIds`, 'unique instance IDs');
  }
  const unavailableReason = record.unavailableReason === null
    ? null
    : parseRuntimeAvailability(record.unavailableReason, `${path}.unavailableReason`);
  const applyReady = booleanAt(record.applyReady, `${path}.applyReady`);
  if (applyReady === (unavailableReason !== null)) {
    return fail(path, 'an exact Apply-ready availability reason');
  }
  if (applyReady && affectedInstanceIds.length > 0) {
    return fail(path, 'no affected instances while Apply-ready');
  }
  return { applyReady, affectedInstanceIds, unavailableReason };
}

export function parseProgramWorkspaceResponse(value: unknown): ProgramWorkspaceResponse {
  const record = objectAt(value, 'programWorkspace');
  exactKeysAt(
    record,
    [
      'schemaVersion',
      'draft',
      'applicability',
      'saved',
      'applied',
      'hasUnsavedChanges',
      'hasUnappliedChanges',
    ],
    'programWorkspace',
  );
  const schemaVersion = stringAt(record.schemaVersion, 'programWorkspace.schemaVersion');
  if (schemaVersion !== 'manyforge.composer_program_workspace.v2') {
    return fail('programWorkspace.schemaVersion', 'the supported workspace version');
  }
  const draft = parseProgramDraftProjection(record.draft, 'programWorkspace.draft');
  const applicability = parseProgramDraftApplicability(
    record.applicability,
    'programWorkspace.applicability',
  );
  const saved = objectAt(record.saved, 'programWorkspace.saved');
  exactKeysAt(
    saved,
    ['available', 'matchesDraft', 'path', 'programRevision'],
    'programWorkspace.saved',
  );
  const savedAvailable = booleanAt(saved.available, 'programWorkspace.saved.available');
  const savedPath = nullableStringAt(saved.path, 'programWorkspace.saved.path');
  const savedRevision = nullableStringAt(
    saved.programRevision,
    'programWorkspace.saved.programRevision',
  );
  const savedMatchesDraft = booleanAt(
    saved.matchesDraft,
    'programWorkspace.saved.matchesDraft',
  );
  if (savedAvailable !== (savedPath !== null && savedRevision !== null)) {
    return fail('programWorkspace.saved', 'internally consistent Saved identity');
  }
  if (
    savedMatchesDraft !== (
      draft.programRevision !== null
      && savedRevision !== null
      && draft.programRevision === savedRevision
    )
  ) {
    return fail('programWorkspace.saved', 'a truthful Draft/Saved comparison');
  }
  const applied = objectAt(record.applied, 'programWorkspace.applied');
  exactKeysAt(
    applied,
    ['available', 'matchesDraft', 'programRevision', 'unavailableReason'],
    'programWorkspace.applied',
  );
  const appliedAvailable = booleanAt(applied.available, 'programWorkspace.applied.available');
  const unavailableReason = applied.unavailableReason === null
    ? null
    : parseRuntimeAvailability(
        applied.unavailableReason,
        'programWorkspace.applied.unavailableReason',
      );
  if (appliedAvailable === (unavailableReason !== null)) {
    return fail('programWorkspace.applied', 'consistent availability and reason');
  }
  const appliedMatchesDraft = applied.matchesDraft === null
    ? null
    : booleanAt(applied.matchesDraft, 'programWorkspace.applied.matchesDraft');
  const appliedRevision = nullableStringAt(
    applied.programRevision,
    'programWorkspace.applied.programRevision',
  );
  if (
    (appliedAvailable && (appliedMatchesDraft === null || appliedRevision === null))
    || (!appliedAvailable && (appliedMatchesDraft !== null || appliedRevision !== null))
  ) {
    return fail('programWorkspace.applied', 'internally consistent Applied identity');
  }
  const hasUnappliedChanges = record.hasUnappliedChanges === null
    ? null
    : booleanAt(record.hasUnappliedChanges, 'programWorkspace.hasUnappliedChanges');
  if (!appliedAvailable && hasUnappliedChanges !== null) {
    return fail(
      'programWorkspace.hasUnappliedChanges',
      'null while Applied identity is unavailable',
    );
  }
  const hasUnsavedChanges = booleanAt(
    record.hasUnsavedChanges,
    'programWorkspace.hasUnsavedChanges',
  );
  if (hasUnsavedChanges !== (draft.programLoaded && !savedMatchesDraft)) {
    return fail(
      'programWorkspace.hasUnsavedChanges',
      'the exact Draft/Saved comparison',
    );
  }
  return {
    schemaVersion,
    draft,
    applicability,
    saved: {
      available: savedAvailable,
      matchesDraft: savedMatchesDraft,
      path: savedPath,
      programRevision: savedRevision,
    },
    applied: {
      available: appliedAvailable,
      matchesDraft: appliedMatchesDraft,
      programRevision: appliedRevision,
      unavailableReason,
    },
    hasUnsavedChanges,
    hasUnappliedChanges,
  };
}

export function parseDeploymentWorkspaceResponse(
  value: unknown,
): DeploymentWorkspaceResponse {
  const path = 'deploymentWorkspace';
  const record = objectAt(value, path);
  exactKeysAt(record, [
    'schemaVersion', 'draft', 'saved', 'managerApplied', 'runAdmission',
    'binding', 'scenarioCandidates', 'savedScenarioCandidates',
    'unavailableReasons',
  ], path);
  const schemaVersion = stringAt(record.schemaVersion, `${path}.schemaVersion`);
  if (schemaVersion !== 'manyforge.composer_deployment_workspace.v1') {
    return fail(`${path}.schemaVersion`, 'the supported Deployment workspace version');
  }
  const draftRecord = objectAt(record.draft, `${path}.draft`);
  exactKeysAt(draftRecord, [
    'id', 'revision', 'document', 'deploymentArtifactId',
    'deploymentRevision', 'graphRevision', 'differsFromSaved',
  ], `${path}.draft`);
  const document = draftRecord.document === null
    ? null
    : jsonRecordAt(draftRecord.document, `${path}.draft.document`);
  const deploymentArtifactId = nullableStringAt(
    draftRecord.deploymentArtifactId,
    `${path}.draft.deploymentArtifactId`,
  );
  const deploymentRevision = draftRecord.deploymentRevision === null
    ? null
    : digestAt(draftRecord.deploymentRevision, `${path}.draft.deploymentRevision`);
  const graphRevision = draftRecord.graphRevision === null
    ? null
    : digestAt(draftRecord.graphRevision, `${path}.draft.graphRevision`);
  const missingDraftIdentities = [
    deploymentArtifactId,
    deploymentRevision,
    graphRevision,
  ].filter((identity) => identity === null).length;
  if (
    (document === null && missingDraftIdentities !== 3)
    || (document !== null && missingDraftIdentities !== 0)
  ) {
    return fail(`${path}.draft`, 'complete identities exactly when a document is loaded');
  }
  if (document !== null) {
    const apiVersion = stringAt(
      document.api_version,
      `${path}.draft.document.api_version`,
    );
    // This is the normalized authored document, not the compiled cell graph.
    // Its graph revision is the compiler-issued sibling above, joined to the
    // Binding below; authored graph content contains no graph_revision field.
    objectAt(document.graph, `${path}.draft.document.graph`);
    if (
      apiVersion !== 'manyforge.deployment.v1'
      || stringAt(
        document.deployment_artifact_id,
        `${path}.draft.document.deployment_artifact_id`,
      ) !== deploymentArtifactId
      || digestAt(
        document.deployment_revision,
        `${path}.draft.document.deployment_revision`,
      ) !== deploymentRevision
    ) {
      return fail(`${path}.draft`, 'one exact compiled Deployment identity');
    }
  }
  const savedRecord = objectAt(record.saved, `${path}.saved`);
  exactKeysAt(savedRecord, ['path', 'deploymentRevision'], `${path}.saved`);
  const savedPath = nullableStringAt(savedRecord.path, `${path}.saved.path`);
  const savedRevision = savedRecord.deploymentRevision === null
    ? null
    : digestAt(savedRecord.deploymentRevision, `${path}.saved.deploymentRevision`);
  if ((savedPath === null) !== (savedRevision === null)) {
    return fail(`${path}.saved`, 'one complete Saved identity');
  }
  const managerRecord = objectAt(record.managerApplied, `${path}.managerApplied`);
  exactKeysAt(managerRecord, ['deploymentRevision', 'scenarioId'], `${path}.managerApplied`);
  const managerRevision = managerRecord.deploymentRevision === null
    ? null
    : digestAt(managerRecord.deploymentRevision, `${path}.managerApplied.deploymentRevision`);
  const managerScenarioId = nullableStringAt(
    managerRecord.scenarioId,
    `${path}.managerApplied.scenarioId`,
  );
  if ((managerRevision === null) !== (managerScenarioId === null)) {
    return fail(`${path}.managerApplied`, 'one complete Manager Applied identity');
  }
  const admissionRecord = objectAt(record.runAdmission, `${path}.runAdmission`);
  exactKeysAt(admissionRecord, ['ready', 'reason'], `${path}.runAdmission`);
  const runReady = booleanAt(admissionRecord.ready, `${path}.runAdmission.ready`);
  const runReason = nullableStringAt(admissionRecord.reason, `${path}.runAdmission.reason`);
  if (runReady === (runReason !== null)) {
    return fail(`${path}.runAdmission`, 'a reason exactly while Run is blocked');
  }
  if (runReady !== (
    deploymentRevision === null || deploymentRevision === managerRevision
  )) {
    return fail(
      `${path}.runAdmission`,
      'the exact Deployment Draft and Manager Applied comparison',
    );
  }
  let binding = null;
  if (record.binding !== null) {
    try {
      binding = parseProgramBindingTransport(record.binding, `${path}.binding`);
    } catch (error) {
      if (error instanceof ProgramBindingTransportError) {
        throw new StructuralContractError(error.message);
      }
      throw error;
    }
  }
  const parseScenarioCandidates = (
    value: unknown,
    field: 'scenarioCandidates' | 'savedScenarioCandidates',
  ) => boundedArrayAt(
    value,
    `${path}.${field}`,
    4096,
  ).map((candidate, index) => {
    const candidatePath = `${path}.${field}[${index}]`;
    const row = objectAt(candidate, candidatePath);
    exactKeysAt(row, [
      'scenarioId', 'scenarioCatalogRevision', 'deploymentArtifactId',
      'deploymentRevision', 'retainedBundleId', 'programId',
      'programRevision', 'programBindingId', 'description',
    ], candidatePath);
    return {
      scenarioId: stringAt(row.scenarioId, `${candidatePath}.scenarioId`),
      scenarioCatalogRevision: digestAt(
        row.scenarioCatalogRevision,
        `${candidatePath}.scenarioCatalogRevision`,
      ),
      deploymentArtifactId: stringAt(
        row.deploymentArtifactId,
        `${candidatePath}.deploymentArtifactId`,
      ),
      deploymentRevision: digestAt(
        row.deploymentRevision,
        `${candidatePath}.deploymentRevision`,
      ),
      retainedBundleId: digestAt(
        row.retainedBundleId,
        `${candidatePath}.retainedBundleId`,
      ),
      programId: stringAt(row.programId, `${candidatePath}.programId`),
      programRevision: digestAt(
        row.programRevision,
        `${candidatePath}.programRevision`,
      ),
      programBindingId: digestAt(
        row.programBindingId,
        `${candidatePath}.programBindingId`,
      ),
      description: nullableStringAt(row.description, `${candidatePath}.description`),
    };
  });
  const scenarioCandidates = parseScenarioCandidates(
    record.scenarioCandidates,
    'scenarioCandidates',
  );
  const savedScenarioCandidates = parseScenarioCandidates(
    record.savedScenarioCandidates,
    'savedScenarioCandidates',
  );
  const scenarioIds = new Set<string>();
  for (const candidate of scenarioCandidates) {
    if (scenarioIds.has(candidate.scenarioId)) {
      return fail(`${path}.scenarioCandidates`, 'unique Scenario identities');
    }
    scenarioIds.add(candidate.scenarioId);
    if (
      candidate.deploymentArtifactId !== deploymentArtifactId
      || candidate.deploymentRevision !== deploymentRevision
    ) {
      return fail(
        `${path}.scenarioCandidates`,
        'only exact matches for the current Deployment Draft',
      );
    }
  }
  if (savedScenarioCandidates.some(
    (candidate) => candidate.deploymentRevision !== savedRevision,
  )) {
    return fail(
      `${path}.savedScenarioCandidates`,
      'only exact matches for the Saved Deployment',
    );
  }
  if (
    binding !== null
    && (
      binding.graph_revision !== graphRevision
      || deploymentRevision !== managerRevision
    )
  ) {
    return fail(
      `${path}.binding`,
      'the exact current Deployment Draft and graph revision',
    );
  }
  return {
    schemaVersion: 'manyforge.composer_deployment_workspace.v1',
    draft: {
      id: stringAt(draftRecord.id, `${path}.draft.id`),
      revision: integerAt(draftRecord.revision, `${path}.draft.revision`),
      document,
      deploymentArtifactId,
      deploymentRevision,
      graphRevision,
      differsFromSaved: booleanAt(
        draftRecord.differsFromSaved,
        `${path}.draft.differsFromSaved`,
      ),
    },
    saved: { path: savedPath, deploymentRevision: savedRevision },
    managerApplied: {
      deploymentRevision: managerRevision,
      scenarioId: managerScenarioId,
    },
    runAdmission: { ready: runReady, reason: runReason },
    binding,
    scenarioCandidates,
    savedScenarioCandidates,
    unavailableReasons: stringArrayAt(
      record.unavailableReasons,
      `${path}.unavailableReasons`,
    ),
  };
}

function parseDeploymentCandidateIdentity(
  value: unknown,
  path: string,
  withSchema: boolean,
): DeploymentCandidateCompilation {
  const record = objectAt(value, path);
  exactKeysAt(record, [
    ...(withSchema ? ['schema_version'] : []),
    'scenario_id', 'scenario_catalog_revision', 'deployment_artifact_id',
    'deployment_revision', 'retained_bundle_id', 'graph_revision',
    'program_id', 'program_revision', 'program_binding_id',
  ], path);
  if (withSchema && stringAt(record.schema_version, `${path}.schema_version`)
    !== 'manyforge.deployment_candidate_compilation.v1') {
    return fail(
      `${path}.schema_version`,
      'manyforge.deployment_candidate_compilation.v1',
    );
  }
  return {
    schemaVersion: 'manyforge.deployment_candidate_compilation.v1',
    scenarioId: stringAt(record.scenario_id, `${path}.scenario_id`),
    scenarioCatalogRevision: digestAt(
      record.scenario_catalog_revision,
      `${path}.scenario_catalog_revision`,
    ),
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
    programId: stringAt(record.program_id, `${path}.program_id`),
    programRevision: digestAt(
      record.program_revision,
      `${path}.program_revision`,
    ),
    programBindingId: digestAt(
      record.program_binding_id,
      `${path}.program_binding_id`,
    ),
  };
}

export function parseDeploymentCandidateOperationResponse(
  value: unknown,
): DeploymentCandidateOperationResponse {
  const path = 'deploymentCandidateOperation';
  const record = objectAt(value, path);
  exactKeysAt(record, ['schemaVersion', 'workspace', 'result'], path);
  const schemaVersion = stringAt(record.schemaVersion, `${path}.schemaVersion`);
  if (schemaVersion !== 'manyforge.composer_deployment_candidate_operation.v1') {
    return fail(
      `${path}.schemaVersion`,
      'manyforge.composer_deployment_candidate_operation.v1',
    );
  }
  const workspace = parseDeploymentWorkspaceResponse(record.workspace);
  const resultRecord = objectAt(record.result, `${path}.result`);
  exactKeysAt(resultRecord, [
    'status', 'compilation', 'candidate_preview', 'manager_apply', 'selection',
  ], `${path}.result`);
  const status = stringAt(resultRecord.status, `${path}.result.status`);
  if (status !== 'previewed' && status !== 'applied') {
    return fail(`${path}.result.status`, 'previewed or applied');
  }
  const compilation = parseDeploymentCandidateIdentity(
    resultRecord.compilation,
    `${path}.result.compilation`,
    true,
  );
  const selection = parseDeploymentCandidateIdentity(
    resultRecord.selection,
    `${path}.result.selection`,
    false,
  );
  const identityFields: Array<keyof Omit<
    DeploymentCandidateCompilation,
    'schemaVersion'
  >> = [
    'scenarioId', 'scenarioCatalogRevision', 'deploymentArtifactId',
    'deploymentRevision', 'retainedBundleId', 'graphRevision', 'programId',
    'programRevision', 'programBindingId',
  ];
  if (identityFields.some((field) => selection[field] !== compilation[field])) {
    return fail(`${path}.result.selection`, 'the exact compiled candidate identity');
  }
  if (
    workspace.draft.deploymentArtifactId !== compilation.deploymentArtifactId
    || workspace.draft.deploymentRevision !== compilation.deploymentRevision
  ) {
    return fail(`${path}.workspace`, 'the exact compiled Deployment Draft');
  }
  const candidatePreview = resultRecord.candidate_preview === null
    ? null
    : parseManagerDeploymentCandidatePreviewResponse(
      resultRecord.candidate_preview,
    );
  const managerApply = resultRecord.manager_apply === null
    ? null
    : parseManagerApplyResponse(resultRecord.manager_apply);
  if (
    candidatePreview !== null
    && (
      candidatePreview.candidateReceipt.deployment_artifact_id
        !== compilation.deploymentArtifactId
      || candidatePreview.candidateReceipt.deployment_revision
        !== compilation.deploymentRevision
      || candidatePreview.candidateReceipt.candidate_retained_bundle_id
        !== compilation.retainedBundleId
      || candidatePreview.candidateReceipt.candidate_graph_revision
        !== compilation.graphRevision
    )
  ) {
    return fail(`${path}.result.candidate_preview`, 'the compiled candidate');
  }
  if (
    status === 'applied'
    && workspace.managerApplied.deploymentRevision
      !== compilation.deploymentRevision
  ) {
    return fail(`${path}.workspace.managerApplied`, 'the applied candidate');
  }
  const selectionWithoutSchema: Omit<
    DeploymentCandidateCompilation,
    'schemaVersion'
  > = {
    scenarioId: selection.scenarioId,
    scenarioCatalogRevision: selection.scenarioCatalogRevision,
    deploymentArtifactId: selection.deploymentArtifactId,
    deploymentRevision: selection.deploymentRevision,
    retainedBundleId: selection.retainedBundleId,
    graphRevision: selection.graphRevision,
    programId: selection.programId,
    programRevision: selection.programRevision,
    programBindingId: selection.programBindingId,
  };
  return {
    schemaVersion,
    workspace,
    result: {
      status,
      compilation,
      candidatePreview,
      managerApply,
      selection: selectionWithoutSchema,
    },
  };
}

export function parseScenarioCatalogResponse(value: unknown): ScenarioCatalogResponse {
  const record = objectAt(value, 'scenarioCatalog');
  exactKeysAt(
    record,
    ['api_version', 'scenario_catalog_revision', 'scenarios'],
    'scenarioCatalog',
  );
  const apiVersion = stringAt(record.api_version, 'scenarioCatalog.api_version');
  if (apiVersion !== 'manyforge.scenario_catalog.v1') {
    return fail('scenarioCatalog.api_version', 'manyforge.scenario_catalog.v1');
  }
  const scenarios = boundedArrayAt(
    record.scenarios,
    'scenarioCatalog.scenarios',
    4096,
  ).map((value, index) => {
    const path = `scenarioCatalog.scenarios[${index}]`;
    const row = objectAt(value, path);
    allowedKeysAt(
      row,
      [
        'assistant_mode',
        'deployment_artifact_id',
        'deployment_compatibility',
        'deployment_revision',
        'program_binding_id',
        'retained_bundle_id',
        'scenario_id',
        'typed_parameter_overrides',
      ],
      ['aliases', 'description'],
      path,
    );
    const compatibility = objectAt(
      row.deployment_compatibility,
      `${path}.deployment_compatibility`,
    );
    exactKeysAt(
      compatibility,
      ['lane', 'provides'],
      `${path}.deployment_compatibility`,
    );
    stringAt(compatibility.lane, `${path}.deployment_compatibility.lane`);
    stringArrayAt(
      compatibility.provides,
      `${path}.deployment_compatibility.provides`,
    );
    jsonRecordAt(row.typed_parameter_overrides, `${path}.typed_parameter_overrides`);
    const assistantMode = stringAt(row.assistant_mode, `${path}.assistant_mode`);
    if (!['required', 'optional', 'forbidden'].includes(assistantMode)) {
      return fail(`${path}.assistant_mode`, 'required, optional, or forbidden');
    }
    return {
      scenarioId: stringAt(row.scenario_id, `${path}.scenario_id`),
      description: row.description === undefined
        ? null
        : stringAt(row.description, `${path}.description`),
      aliases: row.aliases === undefined
        ? []
        : stringArrayAt(row.aliases, `${path}.aliases`),
      deploymentArtifactId: stringAt(
        row.deployment_artifact_id,
        `${path}.deployment_artifact_id`,
      ),
      deploymentRevision: digestAt(
        row.deployment_revision,
        `${path}.deployment_revision`,
      ),
      retainedBundleId: digestAt(
        row.retained_bundle_id,
        `${path}.retained_bundle_id`,
      ),
      programBindingId: digestAt(
        row.program_binding_id,
        `${path}.program_binding_id`,
      ),
      assistantMode: assistantMode as 'required' | 'optional' | 'forbidden',
    };
  });
  const scenarioIds = scenarios.map((row) => row.scenarioId);
  if (
    scenarioIds.some((id, index) => index > 0 && id <= scenarioIds[index - 1])
  ) {
    return fail('scenarioCatalog.scenarios', 'strictly scenario-ID sorted and unique');
  }
  return {
    apiVersion,
    scenarioCatalogRevision: digestAt(
      record.scenario_catalog_revision,
      'scenarioCatalog.scenario_catalog_revision',
    ),
    scenarios,
  };
}

export function parseCurrentScenarioResponse(value: unknown): CurrentScenarioResponse {
  const path = 'currentScenario';
  const record = objectAt(value, path);
  exactKeysAt(
    record,
    ['schema_version', 'ready', 'current_scenario', 'unavailable_reasons'],
    path,
  );
  const schemaVersion = stringAt(record.schema_version, `${path}.schema_version`);
  if (schemaVersion !== 'manyforge.current_scenario.v1') {
    return fail(`${path}.schema_version`, 'the supported current-Scenario version');
  }
  const ready = booleanAt(record.ready, `${path}.ready`);
  const unavailableReasons = stringArrayAt(
    record.unavailable_reasons,
    `${path}.unavailable_reasons`,
  );
  const current = record.current_scenario === null
    ? null
    : objectAt(record.current_scenario, `${path}.current_scenario`);
  if (ready !== (current !== null)) {
    return fail(path, 'a current Scenario exactly when ready');
  }
  if (ready !== (unavailableReasons.length === 0)) {
    return fail(path, 'unavailable reasons exactly when no current Scenario exists');
  }
  if (current === null) {
    return {
      schemaVersion: 'manyforge.current_scenario.v1',
      ready,
      currentScenario: null,
      unavailableReasons,
    };
  }
  exactKeysAt(current, [
    'scenario_id',
    'scenario_catalog_revision',
    'deployment_artifact_id',
    'deployment_revision',
    'retained_bundle_id',
    'graph_revision',
    'program_id',
    'program_revision',
    'program_binding_id',
  ], `${path}.current_scenario`);
  return {
    schemaVersion: 'manyforge.current_scenario.v1',
    ready,
    currentScenario: {
      scenarioId: stringAt(current.scenario_id, `${path}.current_scenario.scenario_id`),
      scenarioCatalogRevision: digestAt(
        current.scenario_catalog_revision,
        `${path}.current_scenario.scenario_catalog_revision`,
      ),
      deploymentArtifactId: stringAt(
        current.deployment_artifact_id,
        `${path}.current_scenario.deployment_artifact_id`,
      ),
      deploymentRevision: digestAt(
        current.deployment_revision,
        `${path}.current_scenario.deployment_revision`,
      ),
      retainedBundleId: digestAt(
        current.retained_bundle_id,
        `${path}.current_scenario.retained_bundle_id`,
      ),
      graphRevision: digestAt(
        current.graph_revision,
        `${path}.current_scenario.graph_revision`,
      ),
      programId: stringAt(current.program_id, `${path}.current_scenario.program_id`),
      programRevision: digestAt(
        current.program_revision,
        `${path}.current_scenario.program_revision`,
      ),
      programBindingId: digestAt(
        current.program_binding_id,
        `${path}.current_scenario.program_binding_id`,
      ),
    },
    unavailableReasons,
  };
}

export function parseAppliedProgramSelection(value: unknown): AppliedProgramSelection {
  const path = 'appliedProgram';
  const record = objectAt(value, path);
  exactKeysAt(
    record,
    [
      'assistant_mode',
      'baseline_program_binding_id',
      'deployment_artifact_id',
      'deployment_revision',
      'graph_revision',
      'node_environment_digest',
      'preparation_digest',
      'program',
      'program_binding',
      'program_binding_id',
      'program_id',
      'program_revision',
      'retained_bundle_id',
      'scenario_catalog_revision',
      'scenario_id',
      'schema_version',
      'source',
      'typed_parameter_overrides',
    ],
    path,
  );
  const schemaVersion = stringAt(record.schema_version, `${path}.schema_version`);
  if (schemaVersion !== 'manyforge.applied_program_selection.v1') {
    return fail(`${path}.schema_version`, 'manyforge.applied_program_selection.v1');
  }
  const assistantMode = stringAt(record.assistant_mode, `${path}.assistant_mode`);
  if (!['required', 'optional', 'forbidden'].includes(assistantMode)) {
    return fail(`${path}.assistant_mode`, 'required, optional, or forbidden');
  }
  digestAt(
    record.baseline_program_binding_id,
    `${path}.baseline_program_binding_id`,
  );
  stringAt(record.deployment_artifact_id, `${path}.deployment_artifact_id`);
  digestAt(record.deployment_revision, `${path}.deployment_revision`);
  digestAt(record.preparation_digest, `${path}.preparation_digest`);
  const program = parseProgramDocument(record.program, `${path}.program`);
  const bindingPath = `${path}.program_binding`;
  let programBinding;
  try {
    programBinding = parseProgramBindingTransport(record.program_binding, bindingPath);
  } catch (error) {
    if (error instanceof ProgramBindingTransportError) {
      throw new StructuralContractError(error.message);
    }
    throw error;
  }
  jsonRecordAt(record.typed_parameter_overrides, `${path}.typed_parameter_overrides`);
  const source = objectAt(record.source, `${path}.source`);
  exactKeysAt(
    source,
    [
      'record_digest',
      'semantic_content_digest',
      'source_generation',
      'source_owner_epoch',
      'source_owner_id',
    ],
    `${path}.source`,
  );
  digestAt(
    source.semantic_content_digest,
    `${path}.source.semantic_content_digest`,
  );
  const programId = stringAt(record.program_id, `${path}.program_id`);
  const programRevision = digestAt(
    record.program_revision,
    `${path}.program_revision`,
  );
  const programBindingId = digestAt(
    record.program_binding_id,
    `${path}.program_binding_id`,
  );
  const retainedBundleId = digestAt(
    record.retained_bundle_id,
    `${path}.retained_bundle_id`,
  );
  const graphRevision = digestAt(
    record.graph_revision,
    `${path}.graph_revision`,
  );
  const nodeEnvironmentDigest = digestAt(
    record.node_environment_digest,
    `${path}.node_environment_digest`,
  );
  if (
    program.program_id !== programId
    || program.program_revision !== programRevision
    || programBinding.program_id !== programId
    || programBinding.program_revision !== programRevision
    || programBinding.program_binding_id !== programBindingId
    || programBinding.retained_bundle_id !== retainedBundleId
    || programBinding.graph_revision !== graphRevision
    || programBinding.node_environment.environment_digest !== nodeEnvironmentDigest
  ) {
    return fail(path, 'content-identical Program and Program Binding identities');
  }
  return {
    schemaVersion,
    scenarioId: stringAt(record.scenario_id, `${path}.scenario_id`),
    scenarioCatalogRevision: digestAt(
      record.scenario_catalog_revision,
      `${path}.scenario_catalog_revision`,
    ),
    programId,
    programRevision,
    programBindingId,
    retainedBundleId,
    graphRevision,
    nodeEnvironmentDigest,
    recordDigest: digestAt(source.record_digest, `${path}.source.record_digest`),
    sourceOwnerId: stringAt(source.source_owner_id, `${path}.source.source_owner_id`),
    sourceOwnerEpoch: stringAt(
      source.source_owner_epoch,
      `${path}.source.source_owner_epoch`,
    ),
    sourceGeneration: positiveIntegerAt(
      source.source_generation,
      `${path}.source.source_generation`,
    ),
    program,
    programBinding,
  };
}

const PROGRAM_RUN_STATES = new Set<ProgramRunStateName>([
  'running',
  'intervention_required',
  'full_stopping',
  'full_stopped',
  'stopping',
  'finalizing',
  'stopped',
  'succeeded',
  'failed',
]);

export function parseProgramRunStatus(value: unknown): ProgramRunStatus {
  const path = 'programRun';
  const record = objectAt(value, path);
  exactKeysAt(
    record,
    [
      'active_operation_count',
      'capture',
      'message',
      'node_status_revision',
      'node_statuses',
      'owner_epoch',
      'owner_id',
      'reason_code',
      'run_revision',
      'schema_version',
      'state',
      'tree_status',
    ],
    path,
  );
  const schemaVersion = stringAt(record.schema_version, `${path}.schema_version`);
  if (schemaVersion !== 'manyforge.program_run_status.v2') {
    return fail(`${path}.schema_version`, 'manyforge.program_run_status.v2');
  }
  const state = stringAt(record.state, `${path}.state`) as ProgramRunStateName;
  if (!PROGRAM_RUN_STATES.has(state)) {
    return fail(`${path}.state`, 'a supported Program-run state');
  }
  const capture = objectAt(record.capture, `${path}.capture`);
  exactKeysAt(
    capture,
    [
      'applied_record_digest',
      'graph_revision',
      'node_environment_digest',
      'node_policies',
      'node_policy_digest',
      'owner_identities',
      'program_binding_id',
      'program_id',
      'program_revision',
      'program_run_id',
      'retained_bundle_id',
      'scenario_catalog_revision',
      'scenario_id',
      'schema_version',
    ],
    `${path}.capture`,
  );
  if (capture.schema_version !== 'manyforge.program_run_capture.v1') {
    return fail(`${path}.capture.schema_version`, 'manyforge.program_run_capture.v1');
  }
  const ownerIdentities = boundedArrayAt(
    capture.owner_identities,
    `${path}.capture.owner_identities`,
    4096,
  ).map((value, index) => {
    const ownerPath = `${path}.capture.owner_identities[${index}]`;
    const owner = objectAt(value, ownerPath);
    exactKeysAt(owner, ['owner_epoch', 'owner_id', 'owner_role'], ownerPath);
    return {
      ownerRole: stringAt(owner.owner_role, `${ownerPath}.owner_role`),
      ownerId: stringAt(owner.owner_id, `${ownerPath}.owner_id`),
      ownerEpoch: stringAt(owner.owner_epoch, `${ownerPath}.owner_epoch`),
    };
  });
  const nodePolicies = boundedArrayAt(
    capture.node_policies,
    `${path}.capture.node_policies`,
    100_000,
  ).map((value, index) => jsonValueAt(
    value,
    `${path}.capture.node_policies[${index}]`,
  ));
  const ownerRoles = ownerIdentities.map((owner) => owner.ownerRole);
  if (
    !ownerRoles.includes('program_runner')
    || ownerRoles.some((role, index) => index > 0 && role <= ownerRoles[index - 1])
  ) {
    return fail(
      `${path}.capture.owner_identities`,
      'strictly role-sorted unique owner identities including program_runner',
    );
  }
  const treeStatus = stringAt(record.tree_status, `${path}.tree_status`);
  if (!['invalid', 'running', 'success', 'failure'].includes(treeStatus)) {
    return fail(`${path}.tree_status`, 'a supported tree status');
  }
  const rawNodeStatuses = objectAt(record.node_statuses, `${path}.node_statuses`);
  const nodeStatusEntries = Object.entries(rawNodeStatuses);
  if (nodeStatusEntries.length > 100_000) {
    return fail(`${path}.node_statuses`, 'at most 100000 authored-node statuses');
  }
  const nodeStatuses: ProgramRunStatus['nodeStatuses'] = {};
  for (const [instanceId, rawStatus] of nodeStatusEntries) {
    const status = stringAt(rawStatus, `${path}.node_statuses.${instanceId}`);
    if (
      instanceId.length === 0
      || instanceId.length > 512
      || !['idle', 'running', 'success', 'failure'].includes(status)
    ) {
      return fail(
        `${path}.node_statuses.${instanceId}`,
        'one bounded authored-node ID and supported presentation status',
      );
    }
    nodeStatuses[instanceId] = status as ProgramRunStatus['nodeStatuses'][string];
  }
  const ownerId = stringAt(record.owner_id, `${path}.owner_id`);
  const ownerEpoch = stringAt(record.owner_epoch, `${path}.owner_epoch`);
  const runnerIdentity = ownerIdentities.find(
    (owner) => owner.ownerRole === 'program_runner',
  );
  if (
    runnerIdentity?.ownerId !== ownerId
    || runnerIdentity.ownerEpoch !== ownerEpoch
  ) {
    return fail(
      `${path}.capture.owner_identities`,
      'a program_runner identity matching the status owner',
    );
  }
  const activeOperationCount = record.active_operation_count === null
    ? null
    : integerAt(record.active_operation_count, `${path}.active_operation_count`);
  if (
    activeOperationCount !== null
    && (activeOperationCount < 0 || activeOperationCount > 4096)
  ) {
    return fail(`${path}.active_operation_count`, 'an integer from 0 through 4096');
  }
  return {
    schemaVersion,
    ownerId,
    ownerEpoch,
    runRevision: positiveIntegerAt(record.run_revision, `${path}.run_revision`),
    state,
    capture: {
      programRunId: stringAt(capture.program_run_id, `${path}.capture.program_run_id`),
      appliedRecordDigest: digestAt(
        capture.applied_record_digest,
        `${path}.capture.applied_record_digest`,
      ),
      scenarioId: stringAt(capture.scenario_id, `${path}.capture.scenario_id`),
      scenarioCatalogRevision: digestAt(
        capture.scenario_catalog_revision,
        `${path}.capture.scenario_catalog_revision`,
      ),
      programId: stringAt(capture.program_id, `${path}.capture.program_id`),
      programRevision: digestAt(
        capture.program_revision,
        `${path}.capture.program_revision`,
      ),
      programBindingId: digestAt(
        capture.program_binding_id,
        `${path}.capture.program_binding_id`,
      ),
      retainedBundleId: digestAt(
        capture.retained_bundle_id,
        `${path}.capture.retained_bundle_id`,
      ),
      graphRevision: digestAt(
        capture.graph_revision,
        `${path}.capture.graph_revision`,
      ),
      nodeEnvironmentDigest: digestAt(
        capture.node_environment_digest,
        `${path}.capture.node_environment_digest`,
      ),
      nodePolicies,
      nodePolicyDigest: digestAt(
        capture.node_policy_digest,
        `${path}.capture.node_policy_digest`,
      ),
      ownerIdentities,
    },
    treeStatus: treeStatus as ProgramRunStatus['treeStatus'],
    nodeStatusRevision: positiveIntegerAt(
      record.node_status_revision,
      `${path}.node_status_revision`,
    ),
    nodeStatuses,
    activeOperationCount,
    reasonCode: nullableStringAt(record.reason_code, `${path}.reason_code`),
    message: stringAt(record.message, `${path}.message`),
  };
}

const RUNTIME_OVERRIDE_STATUSES = new Set<RuntimeOverrideStatus>([
  'staged', 'active', 'cleared', 'stale',
]);

const RECOVERY_ACTION_IDS = new Set<CycleRecoveryActionId>([
  'retry_motion_from_current_state',
  'acknowledge_and_continue',
  'retry_after_scene_repair',
  'replan_from_stopped_state',
  'retry_runtime_step',
  'reset_cycle',
]);

function nullablePositiveIntegerAt(value: unknown, path: string): number | null {
  return value === null ? null : positiveIntegerAt(value, path);
}

function parseRuntimeOverrideRecord(
  value: unknown,
  path: string,
): RuntimeOverrideRecord {
  const record = objectAt(value, path);
  exactKeysAt(record, [
    'schema_version', 'override_id', 'override_revision', 'status',
    'program_run_id', 'program_revision', 'program_binding_id',
    'retained_bundle_id', 'graph_revision', 'node_environment_digest',
    'node_instance_id', 'node_attempt_id', 'parameter_id', 'value',
    'source_observation_id', 'source_owner_generation',
  ], path);
  const schemaVersion = stringAt(record.schema_version, `${path}.schema_version`);
  if (schemaVersion !== 'manyforge.runtime_override.v1') {
    return fail(`${path}.schema_version`, 'manyforge.runtime_override.v1');
  }
  const status = stringAt(record.status, `${path}.status`) as RuntimeOverrideStatus;
  if (!RUNTIME_OVERRIDE_STATUSES.has(status)) {
    return fail(`${path}.status`, 'a supported runtime-override status');
  }
  const sourceObservationId = nullableStringAt(
    record.source_observation_id,
    `${path}.source_observation_id`,
  );
  const sourceOwnerGeneration = nullablePositiveIntegerAt(
    record.source_owner_generation,
    `${path}.source_owner_generation`,
  );
  if ((sourceObservationId === null) !== (sourceOwnerGeneration === null)) {
    return fail(path, 'a paired source observation identity and generation');
  }
  return {
    schemaVersion,
    overrideId: digestAt(record.override_id, `${path}.override_id`),
    overrideRevision: positiveIntegerAt(
      record.override_revision,
      `${path}.override_revision`,
    ),
    status,
    programRunId: stringAt(record.program_run_id, `${path}.program_run_id`),
    programRevision: digestAt(record.program_revision, `${path}.program_revision`),
    programBindingId: digestAt(
      record.program_binding_id,
      `${path}.program_binding_id`,
    ),
    retainedBundleId: digestAt(
      record.retained_bundle_id,
      `${path}.retained_bundle_id`,
    ),
    graphRevision: digestAt(record.graph_revision, `${path}.graph_revision`),
    nodeEnvironmentDigest: digestAt(
      record.node_environment_digest,
      `${path}.node_environment_digest`,
    ),
    nodeInstanceId: stringAt(record.node_instance_id, `${path}.node_instance_id`),
    nodeAttemptId: stringAt(record.node_attempt_id, `${path}.node_attempt_id`),
    parameterId: stringAt(record.parameter_id, `${path}.parameter_id`),
    value: jsonValueAt(record.value, `${path}.value`),
    sourceObservationId,
    sourceOwnerGeneration,
  };
}

function parseRuntimeOverridePromotionProposal(
  value: unknown,
  path: string,
): RuntimeOverridePromotionProposal {
  const record = objectAt(value, path);
  exactKeysAt(record, [
    'schema_version', 'proposal_id', 'status', 'program_run_id',
    'override_id', 'program_revision', 'program_binding_id',
    'node_instance_id', 'node_id', 'parameter_id', 'value',
    'expected_draft_id', 'expected_draft_revision',
  ], path);
  const schemaVersion = stringAt(record.schema_version, `${path}.schema_version`);
  if (schemaVersion !== 'manyforge.runtime_override_promotion.v1') {
    return fail(
      `${path}.schema_version`,
      'manyforge.runtime_override_promotion.v1',
    );
  }
  const status = stringAt(record.status, `${path}.status`);
  if (!['awaiting_reset', 'ready'].includes(status)) {
    return fail(`${path}.status`, 'awaiting_reset or ready');
  }
  return {
    schemaVersion,
    proposalId: digestAt(record.proposal_id, `${path}.proposal_id`),
    status: status as RuntimeOverridePromotionProposal['status'],
    programRunId: stringAt(record.program_run_id, `${path}.program_run_id`),
    overrideId: digestAt(record.override_id, `${path}.override_id`),
    programRevision: digestAt(record.program_revision, `${path}.program_revision`),
    programBindingId: digestAt(
      record.program_binding_id,
      `${path}.program_binding_id`,
    ),
    nodeInstanceId: stringAt(record.node_instance_id, `${path}.node_instance_id`),
    nodeId: stringAt(record.node_id, `${path}.node_id`),
    parameterId: stringAt(record.parameter_id, `${path}.parameter_id`),
    value: jsonValueAt(record.value, `${path}.value`),
    expectedDraftId: stringAt(
      record.expected_draft_id,
      `${path}.expected_draft_id`,
    ),
    expectedDraftRevision: positiveIntegerAt(
      record.expected_draft_revision,
      `${path}.expected_draft_revision`,
    ),
  };
}

function parseRuntimeControlAuditEvent(
  value: unknown,
  path: string,
): RuntimeControlAuditEvent {
  const record = jsonRecordAt(value, path);
  const eventRevision = positiveIntegerAt(
    record.event_revision,
    `${path}.event_revision`,
  );
  const kind = stringAt(record.kind, `${path}.kind`);
  return {
    eventRevision,
    kind,
    facts: Object.fromEntries(
      Object.entries(record).filter(([key]) => (
        key !== 'event_revision' && key !== 'kind'
      )),
    ),
  };
}

export function parseProgramRuntimeControlsSnapshot(
  value: unknown,
  path = 'programRuntimeControls',
): ProgramRuntimeControlsSnapshot {
  const record = objectAt(value, path);
  exactKeysAt(record, [
    'schema_version', 'program_run_id', 'runtime_control_revision',
    'recovery_fences', 'has_active_runtime_overrides',
    'has_staged_runtime_overrides', 'has_stale_runtime_overrides',
    'overrides', 'promotion_proposals', 'retry_counts', 'audit',
  ], path);
  const schemaVersion = stringAt(record.schema_version, `${path}.schema_version`);
  if (schemaVersion !== 'manyforge.program_runtime_controls.v2') {
    return fail(`${path}.schema_version`, 'manyforge.program_runtime_controls.v2');
  }
  const programRunId = stringAt(record.program_run_id, `${path}.program_run_id`);
  const fences = objectAt(record.recovery_fences, `${path}.recovery_fences`);
  exactKeysAt(fences, [
    'program_generation', 'deployment_generation', 'scene_admission_epoch',
    'pose_source_apply_generation',
  ], `${path}.recovery_fences`);
  const overrides = boundedArrayAt(record.overrides, `${path}.overrides`, 1024)
    .map((item, index) => parseRuntimeOverrideRecord(
      item,
      `${path}.overrides[${index}]`,
    ));
  const promotionProposals = boundedArrayAt(
    record.promotion_proposals,
    `${path}.promotion_proposals`,
    1024,
  ).map((item, index) => parseRuntimeOverridePromotionProposal(
    item,
    `${path}.promotion_proposals[${index}]`,
  ));
  const retryRecord = objectAt(record.retry_counts, `${path}.retry_counts`);
  const retryCounts = Object.fromEntries(Object.entries(retryRecord).map(
    ([nodeInstanceId, count]) => [
      stringAt(nodeInstanceId, `${path}.retry_counts key`),
      integerAt(count, `${path}.retry_counts.${nodeInstanceId}`),
    ],
  ));
  if (Object.keys(retryCounts).length > 100_000
      || Object.values(retryCounts).some((count) => count < 0 || count > 255)) {
    return fail(`${path}.retry_counts`, 'at most 100000 Program nodes with retry counts from 0 through 255');
  }
  const revision = positiveIntegerAt(
    record.runtime_control_revision,
    `${path}.runtime_control_revision`,
  );
  const audit = boundedArrayAt(record.audit, `${path}.audit`, 256)
    .map((item, index) => parseRuntimeControlAuditEvent(
      item,
      `${path}.audit[${index}]`,
    ));
  if (
    audit.length !== Math.min(revision - 1, 256)
    || audit.some((event, index) => event.eventRevision !== revision - audit.length + index)
    || new Set(overrides.map((item) => item.overrideId)).size !== overrides.length
    || new Set(promotionProposals.map((item) => item.proposalId)).size
      !== promotionProposals.length
    || overrides.some((item) => item.programRunId !== programRunId)
    || promotionProposals.some((item) => item.programRunId !== programRunId)
    || promotionProposals.some((proposal) => {
      const override = overrides.find(
        (row) => row.overrideId === proposal.overrideId,
      );
      return override === undefined
        || proposal.programRevision !== override.programRevision
        || proposal.programBindingId !== override.programBindingId
        || proposal.nodeInstanceId !== override.nodeInstanceId
        || proposal.parameterId !== override.parameterId
        || JSON.stringify(proposal.value) !== JSON.stringify(override.value);
    })
  ) {
    return fail(path, 'internally consistent run-scoped controls');
  }
  const hasActiveRuntimeOverrides = booleanAt(
    record.has_active_runtime_overrides,
    `${path}.has_active_runtime_overrides`,
  );
  const hasStagedRuntimeOverrides = booleanAt(
    record.has_staged_runtime_overrides,
    `${path}.has_staged_runtime_overrides`,
  );
  const hasStaleRuntimeOverrides = booleanAt(
    record.has_stale_runtime_overrides,
    `${path}.has_stale_runtime_overrides`,
  );
  if (
    hasActiveRuntimeOverrides !== overrides.some((row) => row.status === 'active')
    || hasStagedRuntimeOverrides !== overrides.some((row) => row.status === 'staged')
    || hasStaleRuntimeOverrides !== overrides.some((row) => row.status === 'stale')
  ) {
    return fail(path, 'runtime-override flags matching their retained rows');
  }
  return {
    schemaVersion,
    programRunId,
    runtimeControlRevision: revision,
    recoveryFences: {
      programGeneration: integerAt(
        fences.program_generation,
        `${path}.recovery_fences.program_generation`,
      ),
      deploymentGeneration: integerAt(
        fences.deployment_generation,
        `${path}.recovery_fences.deployment_generation`,
      ),
      sceneAdmissionEpoch: integerAt(
        fences.scene_admission_epoch,
        `${path}.recovery_fences.scene_admission_epoch`,
      ),
      poseSourceApplyGeneration: integerAt(
        fences.pose_source_apply_generation,
        `${path}.recovery_fences.pose_source_apply_generation`,
      ),
    },
    hasActiveRuntimeOverrides,
    hasStagedRuntimeOverrides,
    hasStaleRuntimeOverrides,
    overrides,
    promotionProposals,
    retryCounts,
    audit,
  };
}

function parseCycleRecoveryAction(
  value: unknown,
  path: string,
): CycleRecoveryAction {
  const record = objectAt(value, path);
  exactKeysAt(record, [
    'action_id', 'label_key', 'requires_confirmation',
    'performs_backend_reset', 'enabled', 'disabled_reason_code',
  ], path);
  const actionId = stringAt(
    record.action_id,
    `${path}.action_id`,
  ) as CycleRecoveryActionId;
  if (!RECOVERY_ACTION_IDS.has(actionId)) {
    return fail(`${path}.action_id`, 'a supported recovery action');
  }
  const enabled = booleanAt(record.enabled, `${path}.enabled`);
  const disabledReasonCode = nullableStringAt(
    record.disabled_reason_code,
    `${path}.disabled_reason_code`,
  );
  if ((enabled && disabledReasonCode !== null) || (!enabled && disabledReasonCode === null)) {
    return fail(path, 'an enabled action or an explicit disabled reason');
  }
  return {
    actionId,
    labelKey: stringAt(record.label_key, `${path}.label_key`),
    requiresConfirmation: booleanAt(
      record.requires_confirmation,
      `${path}.requires_confirmation`,
    ),
    performsBackendReset: booleanAt(
      record.performs_backend_reset,
      `${path}.performs_backend_reset`,
    ),
    enabled,
    disabledReasonCode,
  };
}

function parseCycleRecoveryOffer(
  value: unknown,
  path: string,
): CycleRecoveryOffer {
  const record = objectAt(value, path);
  exactKeysAt(record, [
    'schema_version', 'offer_id', 'offer_revision', 'generated_at_ns',
    'valid_until_ns', 'kernel_assessment_revision', 'control_command_epoch',
    'operation_id', 'operation_revision', 'program_run_id',
    'program_binding_id', 'node_instance_id', 'node_attempt_id',
    'participants', 'graph_revision', 'retained_bundle_id', 'cycle_generation',
    'program_generation', 'deployment_generation', 'scene_admission_epoch',
    'pose_source_apply_generation', 'latch_id', 'stop_id', 'stop_generation',
    'stop_certificate_id', 'stop_certificate_revision', 'status',
    'recovery_class', 'actions', 'primary_action', 'reason_code',
    'evidence_refs', 'manual_result', 'policy',
  ], path);
  const schemaVersion = stringAt(record.schema_version, `${path}.schema_version`);
  if (schemaVersion !== 'manyforge.cycle_recovery_offer.v2') {
    return fail(`${path}.schema_version`, 'manyforge.cycle_recovery_offer.v2');
  }
  const actions = boundedArrayAt(record.actions, `${path}.actions`, 32)
    .map((item, index) => parseCycleRecoveryAction(
      item,
      `${path}.actions[${index}]`,
    ));
  if (new Set(actions.map((action) => action.actionId)).size !== actions.length) {
    return fail(`${path}.actions`, 'unique recovery action identities');
  }
  const primaryActionValue = nullableStringAt(
    record.primary_action,
    `${path}.primary_action`,
  );
  const primaryAction = primaryActionValue as CycleRecoveryActionId | null;
  if (
    (primaryAction !== null && !RECOVERY_ACTION_IDS.has(primaryAction))
    || (primaryAction !== null
      && !actions.some((action) => action.actionId === primaryAction && action.enabled))
  ) {
    return fail(`${path}.primary_action`, 'an enabled offered recovery action');
  }
  const manualResult = objectAt(record.manual_result, `${path}.manual_result`);
  exactKeysAt(
    manualResult,
    ['enabled', 'disabled_reason_code'],
    `${path}.manual_result`,
  );
  const manualResultEnabled = booleanAt(
    manualResult.enabled,
    `${path}.manual_result.enabled`,
  );
  const manualResultDisabledReasonCode = nullableStringAt(
    manualResult.disabled_reason_code,
    `${path}.manual_result.disabled_reason_code`,
  );
  if (
    (manualResultEnabled && manualResultDisabledReasonCode !== null)
    || (!manualResultEnabled && manualResultDisabledReasonCode === null)
  ) {
    return fail(
      `${path}.manual_result`,
      'enabled availability or an explicit disabled reason',
    );
  }
  return {
    schemaVersion,
    offerId: digestAt(record.offer_id, `${path}.offer_id`),
    offerRevision: positiveIntegerAt(record.offer_revision, `${path}.offer_revision`),
    generatedAtNs: monotonicNanosecondsAt(
      record.generated_at_ns,
      `${path}.generated_at_ns`,
    ),
    validUntilNs: nullableMonotonicNanosecondsAt(
      record.valid_until_ns,
      `${path}.valid_until_ns`,
    ),
    kernelAssessmentRevision: integerAt(
      record.kernel_assessment_revision,
      `${path}.kernel_assessment_revision`,
    ),
    controlCommandEpoch: integerAt(
      record.control_command_epoch,
      `${path}.control_command_epoch`,
    ),
    operationId: nullableStringAt(record.operation_id, `${path}.operation_id`),
    operationRevision: nullablePositiveIntegerAt(
      record.operation_revision,
      `${path}.operation_revision`,
    ),
    programRunId: stringAt(record.program_run_id, `${path}.program_run_id`),
    programBindingId: digestAt(
      record.program_binding_id,
      `${path}.program_binding_id`,
    ),
    nodeInstanceId: stringAt(record.node_instance_id, `${path}.node_instance_id`),
    nodeAttemptId: stringAt(record.node_attempt_id, `${path}.node_attempt_id`),
    participants: boundedArrayAt(record.participants, `${path}.participants`, 4096)
      .map((item, index) => jsonValueAt(item, `${path}.participants[${index}]`)),
    graphRevision: digestAt(record.graph_revision, `${path}.graph_revision`),
    retainedBundleId: digestAt(
      record.retained_bundle_id,
      `${path}.retained_bundle_id`,
    ),
    cycleGeneration: positiveIntegerAt(
      record.cycle_generation,
      `${path}.cycle_generation`,
    ),
    programGeneration: integerAt(
      record.program_generation,
      `${path}.program_generation`,
    ),
    deploymentGeneration: integerAt(
      record.deployment_generation,
      `${path}.deployment_generation`,
    ),
    sceneAdmissionEpoch: integerAt(
      record.scene_admission_epoch,
      `${path}.scene_admission_epoch`,
    ),
    poseSourceApplyGeneration: integerAt(
      record.pose_source_apply_generation,
      `${path}.pose_source_apply_generation`,
    ),
    latchId: nullableStringAt(record.latch_id, `${path}.latch_id`),
    stopId: nullableStringAt(record.stop_id, `${path}.stop_id`),
    stopGeneration: nullablePositiveIntegerAt(
      record.stop_generation,
      `${path}.stop_generation`,
    ),
    stopCertificateId: nullableStringAt(
      record.stop_certificate_id,
      `${path}.stop_certificate_id`,
    ),
    stopCertificateRevision: nullablePositiveIntegerAt(
      record.stop_certificate_revision,
      `${path}.stop_certificate_revision`,
    ),
    status: stringAt(record.status, `${path}.status`),
    recoveryClass: stringAt(record.recovery_class, `${path}.recovery_class`),
    actions,
    manualResult: {
      enabled: manualResultEnabled,
      disabledReasonCode: manualResultDisabledReasonCode,
    },
    primaryAction,
    reasonCode: stringAt(record.reason_code, `${path}.reason_code`),
    evidenceRefs: stringArrayAt(record.evidence_refs, `${path}.evidence_refs`),
    policy: jsonRecordAt(record.policy, `${path}.policy`),
  };
}

function parseRuntimeOverrideObservationCandidate(
  value: unknown,
  path: string,
): RuntimeOverrideObservationCandidate {
  const record = objectAt(value, path);
  exactKeysAt(record, [
    'schema_version', 'available', 'reason_code', 'reason', 'program_run_id',
    'node_instance_id', 'node_attempt_id', 'node_id', 'parameter_id', 'value',
    'source_observation_id', 'source_owner_generation',
  ], path);
  const schemaVersion = stringAt(record.schema_version, `${path}.schema_version`);
  if (schemaVersion !== 'manyforge.runtime_override_observation_candidate.v1') {
    return fail(
      `${path}.schema_version`,
      'manyforge.runtime_override_observation_candidate.v1',
    );
  }
  const available = booleanAt(record.available, `${path}.available`);
  const result: RuntimeOverrideObservationCandidate = {
    schemaVersion,
    available,
    reasonCode: nullableStringAt(record.reason_code, `${path}.reason_code`),
    reason: nullableStringAt(record.reason, `${path}.reason`),
    programRunId: stringAt(record.program_run_id, `${path}.program_run_id`),
    nodeInstanceId: nullableStringAt(record.node_instance_id, `${path}.node_instance_id`),
    nodeAttemptId: nullableStringAt(record.node_attempt_id, `${path}.node_attempt_id`),
    nodeId: nullableStringAt(record.node_id, `${path}.node_id`),
    parameterId: nullableStringAt(record.parameter_id, `${path}.parameter_id`),
    value: record.value === null ? null : jsonValueAt(record.value, `${path}.value`),
    sourceObservationId: nullableStringAt(
      record.source_observation_id,
      `${path}.source_observation_id`,
    ),
    sourceOwnerGeneration: nullablePositiveIntegerAt(
      record.source_owner_generation,
      `${path}.source_owner_generation`,
    ),
  };
  const exactAvailable = result.nodeInstanceId !== null
    && result.nodeAttemptId !== null
    && result.nodeId !== null
    && result.parameterId !== null
    && result.value !== null
    && result.sourceObservationId !== null
    && result.sourceOwnerGeneration !== null;
  if (
    (available && (!exactAvailable || result.reasonCode !== null || result.reason !== null))
    || (!available && (result.reasonCode === null || result.reason === null))
  ) {
    return fail(path, 'a complete available candidate or an owner reason');
  }
  return result;
}

function parseExecutionSelectionControls(value: unknown, path: string): ProgramExecutionSelectionControls {
  const record = objectAt(value, path);
  exactKeysAt(record, ['selection_revision', 'groups'], path);
  const selectionRevision = integerAt(record.selection_revision, `${path}.selection_revision`);
  if (selectionRevision < 0) return fail(path, 'a nonnegative selection revision');
  let previous = '';
  const groups = boundedArrayAt(record.groups, `${path}.groups`, 4097).map((raw, index) => {
    const key = `${path}.groups[${index}]`;
    const row = objectAt(raw, key);
    exactKeysAt(row, ['group_id', 'node_count', 'held_node_count', 'state',
      'stop_allowed', 'resume_allowed', 'resume_unavailable_reason'], key);
    const groupId = stringAt(row.group_id, `${key}.group_id`);
    if (!/^[a-z][a-z0-9_.-]{0,127}$/.test(groupId) || groupId === 'inherit' || groupId <= previous) {
      return fail(key, 'canonical unique captured group IDs');
    }
    previous = groupId;
    const nodeCount = positiveIntegerAt(row.node_count, `${key}.node_count`);
    const heldNodeCount = integerAt(row.held_node_count, `${key}.held_node_count`);
    const state = stringAt(row.state, `${key}.state`);
    if (!['running', 'held', 'mixed', 'stopping', 'faulted', 'unavailable'].includes(state)) {
      return fail(key, 'an owner selection state');
    }
    const stopAllowed = booleanAt(row.stop_allowed, `${key}.stop_allowed`);
    const resumeAllowed = booleanAt(row.resume_allowed, `${key}.resume_allowed`);
    const resumeUnavailableReason = nullableStringAt(row.resume_unavailable_reason, `${key}.resume_unavailable_reason`);
    if (nodeCount > 100_000 || heldNodeCount < 0 || heldNodeCount > nodeCount
      || (state === 'running' && heldNodeCount !== 0)
      || (state === 'held' && heldNodeCount !== nodeCount)
      || (state === 'mixed' && (heldNodeCount === 0 || heldNodeCount === nodeCount))
      || (resumeAllowed && (!['held', 'mixed'].includes(state) || heldNodeCount === 0))
      || resumeAllowed !== (resumeUnavailableReason === null)) {
      return fail(key, 'coherent bounded selection status and eligibility');
    }
    return { groupId, nodeCount, heldNodeCount,
      state: state as ProgramExecutionSelectionControls['groups'][number]['state'],
      stopAllowed, resumeAllowed, resumeUnavailableReason };
  });
  if (!groups.some((group) => group.groupId === 'all')) return fail(path, 'the built-in all selection');
  return { selectionRevision, groups };
}

export function parseProgramRuntimeControlProjection(
  value: unknown,
): ProgramRuntimeControlProjection {
  const path = 'programRuntimeControlProjection';
  const record = objectAt(value, path);
  exactKeysAt(record, [
    'schema_version', 'owner_id', 'owner_epoch', 'program_run_id',
    'run_revision', 'runtime_controls', 'recovery_offer',
    'observation_override_candidate', 'execution_selections',
  ], path);
  const schemaVersion = stringAt(record.schema_version, `${path}.schema_version`);
  if (schemaVersion !== 'manyforge.program_runtime_control_projection.v4') {
    return fail(
      `${path}.schema_version`,
      'manyforge.program_runtime_control_projection.v4',
    );
  }
  const programRunId = stringAt(record.program_run_id, `${path}.program_run_id`);
  const runtimeControls = parseProgramRuntimeControlsSnapshot(
    record.runtime_controls,
    `${path}.runtime_controls`,
  );
  const recoveryOffer = record.recovery_offer === null
    ? null
    : parseCycleRecoveryOffer(record.recovery_offer, `${path}.recovery_offer`);
  const observationOverrideCandidate = parseRuntimeOverrideObservationCandidate(
    record.observation_override_candidate,
    `${path}.observation_override_candidate`,
  );
  if (
    runtimeControls.programRunId !== programRunId
    || (recoveryOffer !== null && recoveryOffer.programRunId !== programRunId)
    || observationOverrideCandidate.programRunId !== programRunId
  ) {
    return fail(path, 'one exact Program-run identity');
  }
  return {
    schemaVersion,
    ownerId: stringAt(record.owner_id, `${path}.owner_id`),
    ownerEpoch: stringAt(record.owner_epoch, `${path}.owner_epoch`),
    programRunId,
    runRevision: positiveIntegerAt(record.run_revision, `${path}.run_revision`),
    runtimeControls,
    recoveryOffer,
    executionSelections: parseExecutionSelectionControls(record.execution_selections, `${path}.execution_selections`),
    observationOverrideCandidate,
  };
}

export function parseRuntimeOverrideMaterializationResponse(
  value: unknown,
): RuntimeOverrideMaterializationResponse {
  const path = 'runtimeOverrideMaterialization';
  const record = objectAt(value, path);
  exactKeysAt(record, ['proposalId', 'mutation', 'details', 'workspace'], path);
  return {
    proposalId: digestAt(record.proposalId, `${path}.proposalId`),
    mutation: stringAt(record.mutation, `${path}.mutation`),
    details: jsonRecordAt(record.details, `${path}.details`),
    workspace: parseProgramWorkspaceResponse(record.workspace),
  };
}

function parseProgramApplyResult(
  value: unknown,
  path: string,
): Record<string, JsonValue> {
  const record = jsonRecordAt(value, path);
  exactKeysAt(
    record,
    [
      'applied',
      'preparation',
      'schema_version',
      'status',
    ],
    path,
  );
  if (
    stringAt(record.schema_version, `${path}.schema_version`)
    !== 'manyforge.program_apply_result.v2'
  ) {
    return fail(
      `${path}.schema_version`,
      'manyforge.program_apply_result.v2',
    );
  }
  const status = stringAt(record.status, `${path}.status`);
  if (status !== 'applied') {
    return fail(`${path}.status`, 'applied');
  }
  jsonRecordAt(record.preparation, `${path}.preparation`);
  parseAppliedProgramSelection(record.applied);
  return record;
}

function parseScenarioLoadResult(
  value: unknown,
  path: string,
): Record<string, JsonValue> {
  const record = jsonRecordAt(value, path);
  exactKeysAt(record, ['managerApply', 'programApply', 'scenario'], path);
  const scenario = jsonRecordAt(record.scenario, `${path}.scenario`);
  exactKeysAt(
    scenario,
    [
      'deployment_artifact_id',
      'deployment_revision',
      'graph_revision',
      'program_binding_id',
      'program_id',
      'program_revision',
      'retained_bundle_id',
      'scenario_catalog_revision',
      'scenario_id',
    ],
    `${path}.scenario`,
  );
  stringAt(scenario.deployment_artifact_id, `${path}.scenario.deployment_artifact_id`);
  stringAt(scenario.program_id, `${path}.scenario.program_id`);
  stringAt(scenario.scenario_id, `${path}.scenario.scenario_id`);
  for (const field of [
    'deployment_revision',
    'graph_revision',
    'program_binding_id',
    'program_revision',
    'retained_bundle_id',
    'scenario_catalog_revision',
  ]) {
    digestAt(scenario[field], `${path}.scenario.${field}`);
  }
  if (record.managerApply !== null) {
    jsonRecordAt(record.managerApply, `${path}.managerApply`);
  }
  if (record.programApply !== null) {
    parseProgramApplyResult(record.programApply, `${path}.programApply`);
  }
  return record;
}

export function parseProgramWorkspaceTransitionResponse(
  value: unknown,
): ProgramWorkspaceTransitionResponse {
  const path = 'programWorkspaceTransition';
  const record = objectAt(value, path);
  exactKeysAt(record, ['headless', 'status', 'workspace'], path);
  const status = stringAt(record.status, `${path}.status`);
  if (!['applied', 'loaded', 'reverted'].includes(status)) {
    return fail(
      `${path}.status`,
      'applied, loaded, or reverted',
    );
  }
  let headless: Record<string, JsonValue> | null;
  if (record.headless === null) {
    if (status !== 'reverted') {
      return fail(`${path}.headless`, 'an owner response');
    }
    headless = null;
  } else {
    headless = status === 'loaded'
      ? parseScenarioLoadResult(record.headless, `${path}.headless`)
      : parseProgramApplyResult(record.headless, `${path}.headless`);
  }
  return {
    status: status as ProgramWorkspaceTransitionResponse['status'],
    headless,
    workspace: parseProgramWorkspaceResponse(record.workspace),
  };
}

function parseImplementationAvailability(
  value: unknown,
  path: string,
): NodeImplementationAvailability {
  const record = objectAt(value, path);
  exactKeysAt(
    record,
    [
      'adapter_build_qualified',
      'authoring_available',
      'binding_available',
      'engine_id',
      'executable_package_id',
      'execution_available',
      'implementation_id',
      'implementation_identity_resolved',
      'node_id',
      'owner_health_available',
      'reason_codes',
      'restoration_gates',
    ],
    path,
  );
  for (const field of [
    'adapter_build_qualified',
    'binding_available',
    'implementation_identity_resolved',
    'owner_health_available',
  ]) {
    booleanAt(record[field], `${path}.${field}`);
  }
  return {
    nodeId: stringAt(record.node_id, `${path}.node_id`),
    engineId: stringAt(record.engine_id, `${path}.engine_id`),
    implementationId: stringAt(record.implementation_id, `${path}.implementation_id`),
    executablePackageId: stringAt(
      record.executable_package_id,
      `${path}.executable_package_id`,
    ),
    authoringAvailable: booleanAt(record.authoring_available, `${path}.authoring_available`),
    executionAvailable: booleanAt(record.execution_available, `${path}.execution_available`),
    reasonCodes: stringArrayAt(record.reason_codes, `${path}.reason_codes`),
    restorationGates: stringArrayAt(record.restoration_gates, `${path}.restoration_gates`),
  };
}

export function parseNodeEnvironmentResponse(value: unknown): NodeEnvironmentResponse {
  const record = objectAt(value, 'nodeEnvironment');
  allowedKeysAt(
    record,
    [
      'environmentDigest',
      'catalog',
      'executableRegistry',
      'executablePackages',
      'sourceCoverage',
    ],
    ['availability', 'active', 'source'],
    'nodeEnvironment',
  );
  arrayAt(record.executablePackages, 'nodeEnvironment.executablePackages');
  arrayAt(record.sourceCoverage, 'nodeEnvironment.sourceCoverage');
  objectAt(record.catalog, 'nodeEnvironment.catalog');
  const registry = arrayAt(record.executableRegistry, 'nodeEnvironment.executableRegistry');
  const fallbackImplementations = registry.map((item, index) => {
    const path = `nodeEnvironment.executableRegistry[${index}]`;
    const row = objectAt(item, path);
    exactKeysAt(
      row,
      ['node_id', 'engine_id', 'implementation_id', 'executable_package_id'],
      path,
    );
    return {
      nodeId: stringAt(row.node_id, `${path}.node_id`),
      engineId: stringAt(row.engine_id, `${path}.engine_id`),
      implementationId: stringAt(row.implementation_id, `${path}.implementation_id`),
      executablePackageId: stringAt(
        row.executable_package_id,
        `${path}.executable_package_id`,
      ),
      authoringAvailable: true,
      executionAvailable: false,
      reasonCodes: ['active_node_environment_unavailable'],
      restorationGates: [],
    };
  });
  if (record.availability === undefined) {
    return {
      environmentDigest: stringAt(record.environmentDigest, 'nodeEnvironment.environmentDigest'),
      implementations: fallbackImplementations,
      portableAuthoringAvailable: true,
      executionAvailable: false,
      reasonCodes: ['active_node_environment_unavailable'],
      restorationGates: [],
      active: null,
    };
  }
  const availability = objectAt(record.availability, 'nodeEnvironment.availability');
  exactKeysAt(
    availability,
    [
      'binding_context',
      'execution_available',
      'implementations',
      'manager_runtime_converged',
      'portable_authoring_available',
      'reason_codes',
      'restoration_gates',
    ],
    'nodeEnvironment.availability',
  );
  const active = objectAt(record.active, 'nodeEnvironment.active');
  exactKeysAt(active, ['retainedBundleId', 'graphRevision'], 'nodeEnvironment.active');
  return {
    environmentDigest: stringAt(record.environmentDigest, 'nodeEnvironment.environmentDigest'),
    implementations: arrayAt(
      availability.implementations,
      'nodeEnvironment.availability.implementations',
    ).map((item, index) => parseImplementationAvailability(
      item,
      `nodeEnvironment.availability.implementations[${index}]`,
    )),
    portableAuthoringAvailable: booleanAt(
      availability.portable_authoring_available,
      'nodeEnvironment.availability.portable_authoring_available',
    ),
    executionAvailable: booleanAt(
      availability.execution_available,
      'nodeEnvironment.availability.execution_available',
    ),
    reasonCodes: stringArrayAt(
      availability.reason_codes,
      'nodeEnvironment.availability.reason_codes',
    ),
    restorationGates: stringArrayAt(
      availability.restoration_gates,
      'nodeEnvironment.availability.restoration_gates',
    ),
    active: {
      retainedBundleId: stringAt(
        active.retainedBundleId,
        'nodeEnvironment.active.retainedBundleId',
      ),
      graphRevision: stringAt(
        active.graphRevision,
        'nodeEnvironment.active.graphRevision',
      ),
    },
  };
}

function parseSourceModelCoordinate(
  value: unknown,
  path: string,
): SourceModelCoordinate {
  const record = objectAt(value, path);
  exactKeysAt(
    record,
    [
      'coordinate_kind',
      'joint_type',
      'mimic',
      'source_child_link_name',
      'source_joint_index',
      'source_joint_name',
      'source_parent_link_name',
      'unit',
      'viewer_child_link_name',
      'viewer_joint_name',
      'viewer_parent_link_name',
    ],
    path,
  );
  const mimic = record.mimic === null
    ? null
    : (() => {
        const row = objectAt(record.mimic, `${path}.mimic`);
        exactKeysAt(row, ['multiplier', 'offset', 'source_joint_name'], `${path}.mimic`);
        return {
          multiplier: numberAt(row.multiplier, `${path}.mimic.multiplier`),
          offset: numberAt(row.offset, `${path}.mimic.offset`),
          source_joint_name: stringAt(
            row.source_joint_name,
            `${path}.mimic.source_joint_name`,
          ),
        };
      })();
  return {
    coordinate_kind: stringAt(record.coordinate_kind, `${path}.coordinate_kind`),
    joint_type: stringAt(record.joint_type, `${path}.joint_type`),
    mimic,
    source_child_link_name: stringAt(
      record.source_child_link_name,
      `${path}.source_child_link_name`,
    ),
    source_joint_index: integerAt(
      record.source_joint_index,
      `${path}.source_joint_index`,
    ),
    source_joint_name: stringAt(record.source_joint_name, `${path}.source_joint_name`),
    source_parent_link_name: stringAt(
      record.source_parent_link_name,
      `${path}.source_parent_link_name`,
    ),
    unit: stringAt(record.unit, `${path}.unit`),
    viewer_child_link_name: stringAt(
      record.viewer_child_link_name,
      `${path}.viewer_child_link_name`,
    ),
    viewer_joint_name: stringAt(record.viewer_joint_name, `${path}.viewer_joint_name`),
    viewer_parent_link_name: stringAt(
      record.viewer_parent_link_name,
      `${path}.viewer_parent_link_name`,
    ),
  };
}

function parseSourceModelSrdfGroup(
  value: unknown,
  path: string,
): SourceModelSrdfGroup {
  const record = objectAt(value, path);
  exactKeysAt(
    record,
    [
      'chains',
      'source_group_name',
      'source_joint_names',
      'source_link_names',
      'source_subgroup_names',
    ],
    path,
  );
  return {
    chains: boundedArrayAt(record.chains, `${path}.chains`, 4096).map(
      (item, index) => {
        const chainPath = `${path}.chains[${index}]`;
        const chain = objectAt(item, chainPath);
        exactKeysAt(
          chain,
          ['source_base_link_name', 'source_tip_link_name'],
          chainPath,
        );
        return {
          source_base_link_name: stringAt(
            chain.source_base_link_name,
            `${chainPath}.source_base_link_name`,
          ),
          source_tip_link_name: stringAt(
            chain.source_tip_link_name,
            `${chainPath}.source_tip_link_name`,
          ),
        };
      },
    ),
    source_group_name: stringAt(record.source_group_name, `${path}.source_group_name`),
    source_joint_names: stringArrayAt(record.source_joint_names, `${path}.source_joint_names`),
    source_link_names: stringArrayAt(record.source_link_names, `${path}.source_link_names`),
    source_subgroup_names: stringArrayAt(
      record.source_subgroup_names,
      `${path}.source_subgroup_names`,
    ),
  };
}

function parseSourceModelOccurrence(
  value: unknown,
  path: string,
): SourceModelOccurrence {
  const record = objectAt(value, path);
  exactKeysAt(
    record,
    [
      'component_id',
      'coordinates',
      'links',
      'model_asset_id',
      'model_instance_id',
      'model_revision',
      'model_role_id',
      'source_model_name',
      'source_robot_name',
      'srdf_end_effectors',
      'srdf_group_states',
      'srdf_groups',
    ],
    path,
  );
  const coordinates = boundedArrayAt(record.coordinates, `${path}.coordinates`, 100_000)
    .map((item, index) => parseSourceModelCoordinate(item, `${path}.coordinates[${index}]`));
  if (coordinates.some((item, index) => item.source_joint_index !== index)) {
    return fail(`${path}.coordinates`, 'source-indexed coordinates in source order');
  }
  const links = boundedArrayAt(record.links, `${path}.links`, 100_000)
    .map((item, index) => {
      const linkPath = `${path}.links[${index}]`;
      const link = objectAt(item, linkPath);
      exactKeysAt(link, ['source_link_name', 'viewer_link_name'], linkPath);
      return {
        source_link_name: stringAt(link.source_link_name, `${linkPath}.source_link_name`),
        viewer_link_name: stringAt(link.viewer_link_name, `${linkPath}.viewer_link_name`),
      };
    });
  if (
    new Set(links.map((link) => link.source_link_name)).size !== links.length
    || new Set(links.map((link) => link.viewer_link_name)).size !== links.length
    || [...links].sort((left, right) => (
      left.source_link_name < right.source_link_name ? -1
        : left.source_link_name > right.source_link_name ? 1 : 0
    )).some((link, index) => link !== links[index])
  ) {
    return fail(`${path}.links`, 'canonical unique source/viewer link bindings');
  }
  const viewerLinkBySource = new Map(
    links.map((link) => [link.source_link_name, link.viewer_link_name]),
  );
  if (
    new Set(coordinates.map((coordinate) => coordinate.source_joint_name)).size
      !== coordinates.length
    || new Set(coordinates.map((coordinate) => coordinate.viewer_joint_name)).size
      !== coordinates.length
    || coordinates.some((coordinate) => (
      viewerLinkBySource.get(coordinate.source_parent_link_name)
        !== coordinate.viewer_parent_link_name
      || viewerLinkBySource.get(coordinate.source_child_link_name)
        !== coordinate.viewer_child_link_name
    ))
  ) {
    return fail(`${path}.coordinates`, 'exact unique source/viewer coordinate bindings');
  }
  return {
    component_id: nullableStringAt(record.component_id, `${path}.component_id`),
    coordinates,
    links,
    model_asset_id: stringAt(record.model_asset_id, `${path}.model_asset_id`),
    model_instance_id: stringAt(record.model_instance_id, `${path}.model_instance_id`),
    model_revision: stringAt(record.model_revision, `${path}.model_revision`),
    model_role_id: nullableStringAt(record.model_role_id, `${path}.model_role_id`),
    source_model_name: stringAt(record.source_model_name, `${path}.source_model_name`),
    source_robot_name: stringAt(record.source_robot_name, `${path}.source_robot_name`),
    srdf_end_effectors: boundedArrayAt(
      record.srdf_end_effectors,
      `${path}.srdf_end_effectors`,
      4096,
    ).map((item, index) => {
      const rowPath = `${path}.srdf_end_effectors[${index}]`;
      const row = objectAt(item, rowPath);
      exactKeysAt(
        row,
        [
          'source_end_effector_name',
          'source_group_name',
          'source_parent_group_name',
          'source_parent_link_name',
        ],
        rowPath,
      );
      return {
        source_end_effector_name: stringAt(
          row.source_end_effector_name,
          `${rowPath}.source_end_effector_name`,
        ),
        source_group_name: stringAt(row.source_group_name, `${rowPath}.source_group_name`),
        source_parent_group_name: nullableStringAt(
          row.source_parent_group_name,
          `${rowPath}.source_parent_group_name`,
        ),
        source_parent_link_name: stringAt(
          row.source_parent_link_name,
          `${rowPath}.source_parent_link_name`,
        ),
      };
    }),
    srdf_group_states: boundedArrayAt(
      record.srdf_group_states,
      `${path}.srdf_group_states`,
      4096,
    ).map((item, index) => {
      const rowPath = `${path}.srdf_group_states[${index}]`;
      const row = objectAt(item, rowPath);
      exactKeysAt(
        row,
        ['source_group_name', 'source_group_state_name', 'source_joint_names'],
        rowPath,
      );
      return {
        source_group_name: stringAt(row.source_group_name, `${rowPath}.source_group_name`),
        source_group_state_name: stringAt(
          row.source_group_state_name,
          `${rowPath}.source_group_state_name`,
        ),
        source_joint_names: stringArrayAt(
          row.source_joint_names,
          `${rowPath}.source_joint_names`,
        ),
      };
    }),
    srdf_groups: boundedArrayAt(record.srdf_groups, `${path}.srdf_groups`, 4096)
      .map((item, index) => parseSourceModelSrdfGroup(item, `${path}.srdf_groups[${index}]`)),
  };
}

function parseSourceModelTarget(value: unknown, path: string): SourceModelTarget {
  const record = objectAt(value, path);
  exactKeysAt(
    record,
    ['collision_bodies', 'component_id', 'frames', 'model_groups', 'target_id'],
    path,
  );
  return {
    collision_bodies: boundedArrayAt(
      record.collision_bodies,
      `${path}.collision_bodies`,
      4096,
    ).map((item, index) => {
      const rowPath = `${path}.collision_bodies[${index}]`;
      const row = objectAt(item, rowPath);
      exactKeysAt(
        row,
        ['collision_body_role_id', 'model_instance_id', 'source_link_names'],
        rowPath,
      );
      return {
        collision_body_role_id: stringAt(
          row.collision_body_role_id,
          `${rowPath}.collision_body_role_id`,
        ),
        model_instance_id: stringAt(row.model_instance_id, `${rowPath}.model_instance_id`),
        source_link_names: stringArrayAt(row.source_link_names, `${rowPath}.source_link_names`),
      };
    }),
    component_id: stringAt(record.component_id, `${path}.component_id`),
    frames: boundedArrayAt(record.frames, `${path}.frames`, 4096).map((item, index) => {
      const rowPath = `${path}.frames[${index}]`;
      const row = objectAt(item, rowPath);
      exactKeysAt(
        row,
        ['frame_kind', 'frame_role_id', 'model_instance_id', 'source_link_name'],
        rowPath,
      );
      const frameKind = stringAt(row.frame_kind, `${rowPath}.frame_kind`);
      if (!['cell_frame', 'component_model_frame', 'passive_model_frame'].includes(frameKind)) {
        return fail(`${rowPath}.frame_kind`, 'a supported source frame kind');
      }
      return {
        frame_kind: frameKind as SourceModelTarget['frames'][number]['frame_kind'],
        frame_role_id: stringAt(row.frame_role_id, `${rowPath}.frame_role_id`),
        model_instance_id: nullableStringAt(row.model_instance_id, `${rowPath}.model_instance_id`),
        source_link_name: nullableStringAt(row.source_link_name, `${rowPath}.source_link_name`),
      };
    }),
    model_groups: boundedArrayAt(record.model_groups, `${path}.model_groups`, 4096)
      .map((item, index) => {
        const rowPath = `${path}.model_groups[${index}]`;
        const row = objectAt(item, rowPath);
        exactKeysAt(
          row,
          [
            'commandable_source_joint_names',
            'model_group_role_id',
            'model_instance_id',
            'source_group_name',
            'source_joint_names',
            'source_link_names',
          ],
          rowPath,
        );
        return {
          commandable_source_joint_names: stringArrayAt(
            row.commandable_source_joint_names,
            `${rowPath}.commandable_source_joint_names`,
          ),
          model_group_role_id: stringAt(row.model_group_role_id, `${rowPath}.model_group_role_id`),
          model_instance_id: stringAt(row.model_instance_id, `${rowPath}.model_instance_id`),
          source_group_name: stringAt(row.source_group_name, `${rowPath}.source_group_name`),
          source_joint_names: stringArrayAt(row.source_joint_names, `${rowPath}.source_joint_names`),
          source_link_names: stringArrayAt(row.source_link_names, `${rowPath}.source_link_names`),
        };
      }),
    target_id: stringAt(record.target_id, `${path}.target_id`),
  };
}

export function parseSourceModelProjection(value: unknown): SourceModelProjection {
  const record = objectAt(value, 'sourceModel');
  exactKeysAt(
    record,
    [
      'schema_version',
      'cell_id',
      'retained_bundle_id',
      'graph_revision',
      'model_map_contract',
      'model_map_product_id',
      'occurrences',
      'targets',
      'source',
    ],
    'sourceModel',
  );
  const schemaVersion = stringAt(record.schema_version, 'sourceModel.schema_version');
  if (schemaVersion !== 'manyforge.source_model_projection.v2') {
    return fail('sourceModel.schema_version', 'manyforge.source_model_projection.v2');
  }
  const source = objectAt(record.source, 'sourceModel.source');
  exactKeysAt(
    source,
    [
      'available',
      'freshness',
      'known_gaps',
      'projection_generation',
      'projection_owner_epoch',
      'receiver_age_ns',
      'selection_projection_generation',
      'selection_source_content_digest',
      'selection_source_generation',
      'selection_source_owner_epoch',
      'selection_source_owner_id',
      'source_content_digest',
      'source_generation',
      'source_owner_epoch',
      'source_owner_id',
      'source_owner_kind',
    ],
    'sourceModel.source',
  );
  if (source.available !== true || source.freshness !== 'fresh') {
    return fail('sourceModel.source', 'available and fresh');
  }
  if (arrayAt(source.known_gaps, 'sourceModel.source.known_gaps').length !== 0) {
    return fail('sourceModel.source.known_gaps', 'empty');
  }
  if (source.source_owner_kind !== 'source_model_projection') {
    return fail('sourceModel.source.source_owner_kind', 'source_model_projection');
  }
  const occurrences = boundedArrayAt(record.occurrences, 'sourceModel.occurrences', 4096)
    .map((item, index) => parseSourceModelOccurrence(item, `sourceModel.occurrences[${index}]`));
  const occurrenceIds = occurrences.map((item) => item.model_instance_id);
  if (new Set(occurrenceIds).size !== occurrenceIds.length) {
    return fail('sourceModel.occurrences', 'unique model_instance_id values');
  }
  const targets = boundedArrayAt(record.targets, 'sourceModel.targets', 4096)
    .map((item, index) => parseSourceModelTarget(item, `sourceModel.targets[${index}]`));
  let modelMapContract: SourceModelProjection['model_map_contract'] = null;
  let modelMapProductId: string | null = null;
  if (occurrences.length === 0) {
    if (record.model_map_contract !== null || record.model_map_product_id !== null) {
      return fail('sourceModel.model_map_contract', 'null for an empty occurrence set');
    }
  } else {
    const contract = objectAt(record.model_map_contract, 'sourceModel.model_map_contract');
    exactKeysAt(contract, ['contract_id', 'contract_revision'], 'sourceModel.model_map_contract');
    modelMapContract = {
      contract_id: stringAt(contract.contract_id, 'sourceModel.model_map_contract.contract_id'),
      contract_revision: stringAt(
        contract.contract_revision,
        'sourceModel.model_map_contract.contract_revision',
      ),
    };
    modelMapProductId = stringAt(record.model_map_product_id, 'sourceModel.model_map_product_id');
  }
  return {
    schema_version: schemaVersion,
    cell_id: stringAt(record.cell_id, 'sourceModel.cell_id'),
    retained_bundle_id: stringAt(record.retained_bundle_id, 'sourceModel.retained_bundle_id'),
    graph_revision: stringAt(record.graph_revision, 'sourceModel.graph_revision'),
    model_map_contract: modelMapContract,
    model_map_product_id: modelMapProductId,
    occurrences,
    targets,
    source: {
      available: true,
      freshness: 'fresh',
      known_gaps: [],
      projection_generation: integerAt(source.projection_generation, 'sourceModel.source.projection_generation'),
      projection_owner_epoch: stringAt(source.projection_owner_epoch, 'sourceModel.source.projection_owner_epoch'),
      receiver_age_ns: integerAt(source.receiver_age_ns, 'sourceModel.source.receiver_age_ns'),
      selection_projection_generation: integerAt(source.selection_projection_generation, 'sourceModel.source.selection_projection_generation'),
      selection_source_content_digest: stringAt(source.selection_source_content_digest, 'sourceModel.source.selection_source_content_digest'),
      selection_source_generation: integerAt(source.selection_source_generation, 'sourceModel.source.selection_source_generation'),
      selection_source_owner_epoch: stringAt(source.selection_source_owner_epoch, 'sourceModel.source.selection_source_owner_epoch'),
      selection_source_owner_id: stringAt(source.selection_source_owner_id, 'sourceModel.source.selection_source_owner_id'),
      source_content_digest: stringAt(source.source_content_digest, 'sourceModel.source.source_content_digest'),
      source_generation: integerAt(source.source_generation, 'sourceModel.source.source_generation'),
      source_owner_epoch: stringAt(source.source_owner_epoch, 'sourceModel.source.source_owner_epoch'),
      source_owner_id: stringAt(source.source_owner_id, 'sourceModel.source.source_owner_id'),
      source_owner_kind: 'source_model_projection',
    },
  };
}

function parseProjectionRevision(value: unknown, path: string): ProjectionRevision {
  const record = objectAt(value, path);
  exactKeysAt(record, ['retained_bundle_id', 'graph_revision', 'scene_revision'], path);
  const scene = objectAt(record.scene_revision, `${path}.scene_revision`);
  exactKeysAt(scene, ['scene_id', 'revision_id', 'content_hash'], `${path}.scene_revision`);
  return {
    retained_bundle_id: stringAt(record.retained_bundle_id, `${path}.retained_bundle_id`),
    graph_revision: stringAt(record.graph_revision, `${path}.graph_revision`),
    scene_revision: {
      scene_id: stringAt(scene.scene_id, `${path}.scene_revision.scene_id`),
      revision_id: positiveIntegerAt(scene.revision_id, `${path}.scene_revision.revision_id`),
      content_hash: stringAt(scene.content_hash, `${path}.scene_revision.content_hash`),
    },
  };
}

function parseLogicalScenePose(value: unknown, path: string): LogicalScenePose {
  const record = objectAt(value, path);
  exactKeysAt(record, ['position_m', 'orientation_xyzw'], path);
  const position = objectAt(record.position_m, `${path}.position_m`);
  exactKeysAt(position, ['x', 'y', 'z'], `${path}.position_m`);
  return {
    position_m: {
      x: numberAt(position.x, `${path}.position_m.x`),
      y: numberAt(position.y, `${path}.position_m.y`),
      z: numberAt(position.z, `${path}.position_m.z`),
    },
    orientation_xyzw: quaternionAt(record.orientation_xyzw, `${path}.orientation_xyzw`),
  };
}

function parseLogicalSceneShape(value: unknown, path: string): LogicalSceneShape {
  const record = objectAt(value, path);
  const type = stringAt(record.type, `${path}.type`);
  if (type === 'box') {
    exactKeysAt(record, ['dimensions_m', 'type'], path);
    const dimensions = objectAt(record.dimensions_m, `${path}.dimensions_m`);
    exactKeysAt(dimensions, ['x', 'y', 'z'], `${path}.dimensions_m`);
    return {
      type,
      dimensions_m: {
        x: positiveNumberAt(dimensions.x, `${path}.dimensions_m.x`),
        y: positiveNumberAt(dimensions.y, `${path}.dimensions_m.y`),
        z: positiveNumberAt(dimensions.z, `${path}.dimensions_m.z`),
      },
    };
  }
  if (type === 'sphere') {
    exactKeysAt(record, ['radius_m', 'type'], path);
    return { type, radius_m: positiveNumberAt(record.radius_m, `${path}.radius_m`) };
  }
  if (type === 'cylinder') {
    exactKeysAt(record, ['height_m', 'radius_m', 'type'], path);
    return {
      type,
      radius_m: positiveNumberAt(record.radius_m, `${path}.radius_m`),
      height_m: positiveNumberAt(record.height_m, `${path}.height_m`),
    };
  }
  if (type === 'mesh') {
    exactKeysAt(record, ['resource_uri', 'scale', 'type'], path);
    const scale = objectAt(record.scale, `${path}.scale`);
    exactKeysAt(scale, ['x', 'y', 'z'], `${path}.scale`);
    return {
      type,
      resource_uri: stringAt(record.resource_uri, `${path}.resource_uri`),
      scale: {
        x: positiveNumberAt(scale.x, `${path}.scale.x`),
        y: positiveNumberAt(scale.y, `${path}.scale.y`),
        z: positiveNumberAt(scale.z, `${path}.scale.z`),
      },
    };
  }
  return fail(`${path}.type`, 'box, sphere, cylinder, or mesh');
}

function parseLogicalSceneObject(value: unknown, path: string): LogicalSceneObject {
  const record = objectAt(value, path);
  exactKeysAt(
    record,
    ['collision_shapes', 'placement', 'provider_evidence', 'scene_resource_id', 'support_contacts'],
    path,
  );
  const placementRecord = objectAt(record.placement, `${path}.placement`);
  const kind = stringAt(placementRecord.kind, `${path}.placement.kind`);
  let placement: LogicalScenePlacement;
  if (kind === 'cell_fixed') {
    exactKeysAt(placementRecord, ['cell_id', 'kind', 'pose_in_cell'], `${path}.placement`);
    placement = {
      kind,
      cell_id: stringAt(placementRecord.cell_id, `${path}.placement.cell_id`),
      pose_in_cell: parseLogicalScenePose(
        placementRecord.pose_in_cell,
        `${path}.placement.pose_in_cell`,
      ),
    };
  } else if (kind === 'carried') {
    exactKeysAt(
      placementRecord,
      ['carried_object_attachment_id', 'carried_object_attachment_revision', 'kind'],
      `${path}.placement`,
    );
    placement = {
      kind,
      carried_object_attachment_id: stringAt(
        placementRecord.carried_object_attachment_id,
        `${path}.placement.carried_object_attachment_id`,
      ),
      carried_object_attachment_revision: stringAt(
        placementRecord.carried_object_attachment_revision,
        `${path}.placement.carried_object_attachment_revision`,
      ),
    };
  } else {
    return fail(`${path}.placement.kind`, 'cell_fixed or carried');
  }
  return {
    scene_resource_id: stringAt(record.scene_resource_id, `${path}.scene_resource_id`),
    collision_shapes: boundedArrayAt(record.collision_shapes, `${path}.collision_shapes`, 4096)
      .map((item, index) => parseLogicalSceneShape(item, `${path}.collision_shapes[${index}]`)),
    placement,
    provider_evidence: record.provider_evidence === null
      ? null
      : jsonRecordAt(record.provider_evidence, `${path}.provider_evidence`),
    support_contacts: boundedArrayAt(record.support_contacts, `${path}.support_contacts`, 4096)
      .map((item, index) => jsonRecordAt(item, `${path}.support_contacts[${index}]`)),
  };
}

function parseCarriedObjectAttachment(
  value: unknown,
  path: string,
): CarriedObjectAttachment {
  const record = objectAt(value, path);
  exactKeysAt(record, [
    'carried_object_attachment_id',
    'carried_object_attachment_revision',
    'scene_resource_id',
    'controllable_component_id',
    'command_target_id',
    'carrier_frame_ref',
    'pose_in_carrier_frame',
    'touch_body_roles',
  ], path);
  const frame = objectAt(record.carrier_frame_ref, `${path}.carrier_frame_ref`);
  exactKeysAt(frame, [
    'kind',
    'controllable_component_id',
    'model_instance_id',
    'frame_role_id',
  ], `${path}.carrier_frame_ref`);
  if (stringAt(frame.kind, `${path}.carrier_frame_ref.kind`) !== 'component') {
    return fail(`${path}.carrier_frame_ref.kind`, 'component');
  }
  const pose = objectAt(record.pose_in_carrier_frame, `${path}.pose_in_carrier_frame`);
  exactKeysAt(pose, ['position_m', 'orientation_xyzw'], `${path}.pose_in_carrier_frame`);
  const position = objectAt(pose.position_m, `${path}.pose_in_carrier_frame.position_m`);
  exactKeysAt(position, ['x', 'y', 'z'], `${path}.pose_in_carrier_frame.position_m`);
  const touchBodyRoles = boundedArrayAt(record.touch_body_roles, `${path}.touch_body_roles`, 4096)
    .map((item, index) => stringAt(item, `${path}.touch_body_roles[${index}]`));
  if (JSON.stringify(touchBodyRoles) !== JSON.stringify([...new Set(touchBodyRoles)].sort())) {
    return fail(`${path}.touch_body_roles`, 'sorted unique role IDs');
  }
  return {
    carried_object_attachment_id: stringAt(
      record.carried_object_attachment_id,
      `${path}.carried_object_attachment_id`,
    ),
    carried_object_attachment_revision: digestAt(
      record.carried_object_attachment_revision,
      `${path}.carried_object_attachment_revision`,
    ),
    scene_resource_id: stringAt(record.scene_resource_id, `${path}.scene_resource_id`),
    controllable_component_id: stringAt(
      record.controllable_component_id,
      `${path}.controllable_component_id`,
    ),
    command_target_id: stringAt(record.command_target_id, `${path}.command_target_id`),
    carrier_frame_ref: {
      kind: 'component',
      controllable_component_id: stringAt(
        frame.controllable_component_id,
        `${path}.carrier_frame_ref.controllable_component_id`,
      ),
      model_instance_id: stringAt(
        frame.model_instance_id,
        `${path}.carrier_frame_ref.model_instance_id`,
      ),
      frame_role_id: stringAt(frame.frame_role_id, `${path}.carrier_frame_ref.frame_role_id`),
    },
    pose_in_carrier_frame: {
      position_m: {
        x: numberAt(position.x, `${path}.pose_in_carrier_frame.position_m.x`),
        y: numberAt(position.y, `${path}.pose_in_carrier_frame.position_m.y`),
        z: numberAt(position.z, `${path}.pose_in_carrier_frame.position_m.z`),
      },
      orientation_xyzw: quaternionAt(
        pose.orientation_xyzw,
        `${path}.pose_in_carrier_frame.orientation_xyzw`,
      ),
    },
    touch_body_roles: touchBodyRoles,
  };
}

export function parseSceneMeshAssetMetadata(
  value: unknown,
): SceneMeshAssetMetadata {
  const record = objectAt(value, 'sceneMeshAsset');
  exactKeysAt(
    record,
    [
      'asset_hash',
      'cell_id',
      'last_error',
      'mesh_asset_status',
      'mesh_asset_url',
      'schema_version',
      'source_generation',
      'source_owner_epoch',
    ],
    'sceneMeshAsset',
  );
  const schemaVersion = stringAt(
    record.schema_version,
    'sceneMeshAsset.schema_version',
  );
  if (schemaVersion !== 'manyforge.scene_mesh_asset.v1') {
    return fail(
      'sceneMeshAsset.schema_version',
      'manyforge.scene_mesh_asset.v1',
    );
  }
  const assetHash = stringAt(record.asset_hash, 'sceneMeshAsset.asset_hash');
  if (!/^[0-9a-f]{16}$/.test(assetHash)) {
    return fail('sceneMeshAsset.asset_hash', '16 lowercase hexadecimal characters');
  }
  const status = stringAt(
    record.mesh_asset_status,
    'sceneMeshAsset.mesh_asset_status',
  );
  if (!['pending', 'ok', 'parse_failed'].includes(status)) {
    return fail(
      'sceneMeshAsset.mesh_asset_status',
      'pending, ok, or parse_failed',
    );
  }
  const lastError = nullableStringAt(
    record.last_error,
    'sceneMeshAsset.last_error',
  );
  if ((status === 'parse_failed') !== (lastError !== null)) {
    return fail(
      'sceneMeshAsset.last_error',
      'present exactly for parse_failed status',
    );
  }
  const sourceOwnerEpoch = stringAt(
    record.source_owner_epoch,
    'sceneMeshAsset.source_owner_epoch',
  );
  if (!/^[A-Za-z0-9][A-Za-z0-9._:-]{0,255}$/.test(sourceOwnerEpoch)) {
    return fail('sceneMeshAsset.source_owner_epoch', 'a bounded owner epoch');
  }
  const sourceGeneration = integerAt(
    record.source_generation,
    'sceneMeshAsset.source_generation',
  );
  const cellId = stringAt(record.cell_id, 'sceneMeshAsset.cell_id');
  if (!cellId || cellId.length > 256) {
    return fail('sceneMeshAsset.cell_id', 'a bounded non-empty cell ID');
  }
  const meshAssetUrl = stringAt(
    record.mesh_asset_url,
    'sceneMeshAsset.mesh_asset_url',
  );
  const expectedParams = new URLSearchParams({
    owner_epoch: sourceOwnerEpoch,
    generation: String(sourceGeneration),
  });
  if (
    meshAssetUrl
    !== `/api/scene/object-meshes/${assetHash}.glb?${expectedParams.toString()}`
  ) {
    return fail(
      'sceneMeshAsset.mesh_asset_url',
      'the exact owner-generation-bound asset URL',
    );
  }
  return {
    schema_version: schemaVersion,
    asset_hash: assetHash,
    mesh_asset_url: meshAssetUrl,
    mesh_asset_status: status as SceneMeshAssetMetadata['mesh_asset_status'],
    source_owner_epoch: sourceOwnerEpoch,
    source_generation: sourceGeneration,
    cell_id: cellId,
    last_error: lastError,
  };
}

export function parseProgramDraftMeshAssetMetadata(
  value: unknown,
): ProgramDraftMeshAssetMetadata {
  const record = objectAt(value, 'programDraftMeshAsset');
  exactKeysAt(
    record,
    [
      'asset_hash',
      'draft_id',
      'draft_revision',
      'last_error',
      'mesh_asset_status',
      'mesh_asset_url',
      'schema_version',
    ],
    'programDraftMeshAsset',
  );
  const schemaVersion = stringAt(
    record.schema_version,
    'programDraftMeshAsset.schema_version',
  );
  if (schemaVersion !== 'manyforge.program_draft_mesh_asset.v1') {
    return fail(
      'programDraftMeshAsset.schema_version',
      'manyforge.program_draft_mesh_asset.v1',
    );
  }
  const assetHash = stringAt(
    record.asset_hash,
    'programDraftMeshAsset.asset_hash',
  );
  if (!/^[0-9a-f]{16}$/.test(assetHash)) {
    return fail(
      'programDraftMeshAsset.asset_hash',
      '16 lowercase hexadecimal characters',
    );
  }
  const status = stringAt(
    record.mesh_asset_status,
    'programDraftMeshAsset.mesh_asset_status',
  );
  if (!['pending', 'ok', 'parse_failed'].includes(status)) {
    return fail(
      'programDraftMeshAsset.mesh_asset_status',
      'pending, ok, or parse_failed',
    );
  }
  const lastError = nullableStringAt(
    record.last_error,
    'programDraftMeshAsset.last_error',
  );
  if ((status === 'parse_failed') !== (lastError !== null)) {
    return fail(
      'programDraftMeshAsset.last_error',
      'present exactly for parse_failed status',
    );
  }
  const draftId = stringAt(record.draft_id, 'programDraftMeshAsset.draft_id');
  if (!draftId || draftId.length > 256) {
    return fail(
      'programDraftMeshAsset.draft_id',
      'a bounded non-empty Draft ID',
    );
  }
  const draftRevision = integerAt(
    record.draft_revision,
    'programDraftMeshAsset.draft_revision',
  );
  if (draftRevision < 0) {
    return fail('programDraftMeshAsset.draft_revision', 'a non-negative integer');
  }
  const meshAssetUrl = stringAt(
    record.mesh_asset_url,
    'programDraftMeshAsset.mesh_asset_url',
  );
  const expectedParams = new URLSearchParams({
    draft_id: draftId,
    draft_revision: String(draftRevision),
  });
  if (
    meshAssetUrl
    !== `/api/program/draft/scene-meshes/${assetHash}.glb?${expectedParams.toString()}`
  ) {
    return fail(
      'programDraftMeshAsset.mesh_asset_url',
      'the exact Draft-revision-bound asset URL',
    );
  }
  return {
    schema_version: schemaVersion,
    asset_hash: assetHash,
    mesh_asset_url: meshAssetUrl,
    mesh_asset_status: status as ProgramDraftMeshAssetMetadata['mesh_asset_status'],
    draft_id: draftId,
    draft_revision: draftRevision,
    last_error: lastError,
  };
}

export function parseLogicalSceneProjection(value: unknown): LogicalSceneProjection {
  const record = objectAt(value, 'logicalScene');
  exactKeysAt(record, ['access', 'api_version', 'snapshot'], 'logicalScene');
  const apiVersion = stringAt(record.api_version, 'logicalScene.api_version');
  if (apiVersion !== 'manyforge.logical_scene_source.v1') {
    return fail('logicalScene.api_version', 'manyforge.logical_scene_source.v1');
  }
  const access = objectAt(record.access, 'logicalScene.access');
  exactKeysAt(
    access,
    ['active_projection', 'phase', 'projection_leases_open', 'scene_writes_open'],
    'logicalScene.access',
  );
  const activeProjection = access.active_projection === null
    ? null
    : parseProjectionRevision(access.active_projection, 'logicalScene.access.active_projection');
  if (record.snapshot === null) {
    if (activeProjection !== null) {
      return fail('logicalScene', 'matching null active projection and snapshot');
    }
    return {
      api_version: apiVersion,
      access: {
        active_projection: null,
        phase: stringAt(access.phase, 'logicalScene.access.phase'),
        projection_leases_open: booleanAt(
          access.projection_leases_open,
          'logicalScene.access.projection_leases_open',
        ),
        scene_writes_open: booleanAt(access.scene_writes_open, 'logicalScene.access.scene_writes_open'),
      },
      snapshot: null,
    };
  }
  if (activeProjection === null) {
    return fail('logicalScene', 'matching active projection and snapshot');
  }
  const snapshot = objectAt(record.snapshot, 'logicalScene.snapshot');
  exactKeysAt(
    snapshot,
    [
      'carried_object_attachments',
      'cell_id',
      'configured_collision_policy',
      'objects',
      'policy_epoch',
      'revision',
      'topology_epoch',
      'transaction_sequence',
    ],
    'logicalScene.snapshot',
  );
  const snapshotRevisionRecord = objectAt(snapshot.revision, 'logicalScene.snapshot.revision');
  exactKeysAt(
    snapshotRevisionRecord,
    ['scene_id', 'revision_id', 'content_hash'],
    'logicalScene.snapshot.revision',
  );
  const snapshotRevision = {
    scene_id: stringAt(snapshotRevisionRecord.scene_id, 'logicalScene.snapshot.revision.scene_id'),
    revision_id: positiveIntegerAt(snapshotRevisionRecord.revision_id, 'logicalScene.snapshot.revision.revision_id'),
    content_hash: stringAt(snapshotRevisionRecord.content_hash, 'logicalScene.snapshot.revision.content_hash'),
  };
  if (JSON.stringify(snapshotRevision) !== JSON.stringify(activeProjection.scene_revision)) {
    return fail('logicalScene.snapshot.revision', 'the active projection scene revision');
  }
  const cellId = stringAt(snapshot.cell_id, 'logicalScene.snapshot.cell_id');
  const objects = boundedArrayAt(snapshot.objects, 'logicalScene.snapshot.objects', 4096)
    .map((item, index) => parseLogicalSceneObject(item, `logicalScene.snapshot.objects[${index}]`));
  if (objects.some((item) => item.placement.kind === 'cell_fixed' && item.placement.cell_id !== cellId)) {
    return fail('logicalScene.snapshot.objects', 'cell-fixed placements in the snapshot cell');
  }
  return {
    api_version: apiVersion,
    access: {
      active_projection: activeProjection,
      phase: stringAt(access.phase, 'logicalScene.access.phase'),
      projection_leases_open: booleanAt(access.projection_leases_open, 'logicalScene.access.projection_leases_open'),
      scene_writes_open: booleanAt(access.scene_writes_open, 'logicalScene.access.scene_writes_open'),
    },
    snapshot: {
      carried_object_attachments: boundedArrayAt(
        snapshot.carried_object_attachments,
        'logicalScene.snapshot.carried_object_attachments',
        4096,
      ).map((item, index) => parseCarriedObjectAttachment(
        item,
        `logicalScene.snapshot.carried_object_attachments[${index}]`,
      )),
      cell_id: cellId,
      configured_collision_policy: jsonRecordAt(
        snapshot.configured_collision_policy,
        'logicalScene.snapshot.configured_collision_policy',
      ),
      objects,
      policy_epoch: integerAt(snapshot.policy_epoch, 'logicalScene.snapshot.policy_epoch'),
      revision: snapshotRevision,
      topology_epoch: integerAt(snapshot.topology_epoch, 'logicalScene.snapshot.topology_epoch'),
      transaction_sequence: integerAt(
        snapshot.transaction_sequence,
        'logicalScene.snapshot.transaction_sequence',
      ),
    },
  };
}

export function parseSceneRuntimeProjection(value: unknown): SceneRuntimeProjection {
  const record = objectAt(value, 'sceneRuntime');
  exactKeysAt(record, ['api_version', 'cell_id', 'state'], 'sceneRuntime');
  const apiVersion = stringAt(record.api_version, 'sceneRuntime.api_version');
  if (apiVersion !== 'manyforge.scene_runtime_source.v1') {
    return fail('sceneRuntime.api_version', 'manyforge.scene_runtime_source.v1');
  }
  const state = objectAt(record.state, 'sceneRuntime.state');
  exactKeysAt(
    state,
    [
      'active_replay_id',
      'api_version',
      'counters',
      'deferred',
      'error',
      'initialization_generation',
      'initialization_token',
      'pose_sources',
      'reason',
      'runtime_identity',
      'started_at_ns',
      'state',
      'updated_at_ns',
    ],
    'sceneRuntime.state',
  );
  const stateApiVersion = stringAt(state.api_version, 'sceneRuntime.state.api_version');
  if (stateApiVersion !== 'manyforge.scene_runtime_state.v1') {
    return fail('sceneRuntime.state.api_version', 'manyforge.scene_runtime_state.v1');
  }
  const counters = objectAt(state.counters, 'sceneRuntime.state.counters');
  exactKeysAt(
    counters,
    [
      'buffer_replaced',
      'buffered',
      'closed',
      'failed',
      'opened',
      'replayed',
      'retired',
      'stale_completion_rejected',
    ],
    'sceneRuntime.state.counters',
  );
  const parsedCounters = Object.fromEntries(
    Object.entries(counters).map(([key, item]) => [
      key,
      integerAt(item, `sceneRuntime.state.counters.${key}`),
    ]),
  );
  const stateName = stringAt(state.state, 'sceneRuntime.state.state');
  const stateNames = ['open', 'closed', 'resetting', 'seeding', 'replaying', 'failed', 'retired'] as const;
  if (!stateNames.includes(stateName as typeof stateNames[number])) {
    return fail('sceneRuntime.state.state', 'a supported scene-runtime state');
  }
  let runtimeIdentity: SceneRuntimeProjection['state']['runtime_identity'] = null;
  if (state.runtime_identity !== null) {
    const identity = objectAt(state.runtime_identity, 'sceneRuntime.state.runtime_identity');
    exactKeysAt(
      identity,
      ['deployment_generation', 'graph_revision', 'program_generation', 'retained_bundle_id', 'runtime_generation'],
      'sceneRuntime.state.runtime_identity',
    );
    runtimeIdentity = {
      deployment_generation: integerAt(identity.deployment_generation, 'sceneRuntime.state.runtime_identity.deployment_generation'),
      graph_revision: stringAt(identity.graph_revision, 'sceneRuntime.state.runtime_identity.graph_revision'),
      program_generation: integerAt(identity.program_generation, 'sceneRuntime.state.runtime_identity.program_generation'),
      retained_bundle_id: stringAt(identity.retained_bundle_id, 'sceneRuntime.state.runtime_identity.retained_bundle_id'),
      runtime_generation: positiveIntegerAt(identity.runtime_generation, 'sceneRuntime.state.runtime_identity.runtime_generation'),
    };
  }
  return {
    api_version: apiVersion,
    cell_id: stringAt(record.cell_id, 'sceneRuntime.cell_id'),
    state: {
      api_version: stateApiVersion,
      active_replay_id: state.active_replay_id === null
        ? null
        : positiveIntegerAt(state.active_replay_id, 'sceneRuntime.state.active_replay_id'),
      counters: parsedCounters,
      deferred: booleanAt(state.deferred, 'sceneRuntime.state.deferred'),
      error: nullableStringAt(state.error, 'sceneRuntime.state.error'),
      initialization_generation: integerAt(
        state.initialization_generation,
        'sceneRuntime.state.initialization_generation',
      ),
      initialization_token: nullableStringAt(
        state.initialization_token,
        'sceneRuntime.state.initialization_token',
      ),
      pose_sources: boundedArrayAt(
        state.pose_sources,
        'sceneRuntime.state.pose_sources',
        4096,
      ).map((item, index) => parseSceneRuntimePoseSource(
        item,
        `sceneRuntime.state.pose_sources[${index}]`,
      )),
      reason: stringAt(state.reason, 'sceneRuntime.state.reason'),
      runtime_identity: runtimeIdentity,
      started_at_ns: monotonicNanosecondsAt(state.started_at_ns, 'sceneRuntime.state.started_at_ns'),
      state: stateName as SceneRuntimeProjection['state']['state'],
      updated_at_ns: monotonicNanosecondsAt(state.updated_at_ns, 'sceneRuntime.state.updated_at_ns'),
    },
  };
}

function nonEmptyStringAt(value: unknown, path: string): string {
  const parsed = stringAt(value, path);
  if (parsed.length === 0) return fail(path, 'a non-empty string');
  return parsed;
}

function parseSceneRuntimeObservationPose(
  value: unknown,
  path: string,
): SceneRuntimeObservationPose {
  const record = objectAt(value, path);
  exactKeysAt(
    record,
    [
      'frame_id',
      'frame_kind',
      'orientation_xyzw',
      'position_m',
      'source_stamp_ns',
    ],
    path,
  );
  if (record.frame_kind !== 'cell_frame') {
    return fail(`${path}.frame_kind`, 'the literal "cell_frame"');
  }
  const orientation = quaternionAt(
    record.orientation_xyzw,
    `${path}.orientation_xyzw`,
  );
  if (Math.abs(Math.hypot(...orientation) - 1) > 1e-6) {
    return fail(`${path}.orientation_xyzw`, 'a normalized xyzw quaternion');
  }
  return {
    frame_id: nonEmptyStringAt(record.frame_id, `${path}.frame_id`),
    frame_kind: 'cell_frame',
    orientation_xyzw: orientation,
    position_m: vector3At(record.position_m, `${path}.position_m`),
    source_stamp_ns: record.source_stamp_ns === null ? null : parseNanoseconds(
      record.source_stamp_ns,
      `${path}.source_stamp_ns`,
    ),
  };
}

function parseSceneRuntimeGoodPayload(
  value: unknown,
  path: string,
): SceneRuntimeGoodObservationPayload {
  const record = objectAt(value, path);
  if (record.api_version !== 'manyforge.ros_pose_observation.v2') {
    return fail(`${path}.api_version`, 'manyforge.ros_pose_observation.v2');
  }
  const kind = stringAt(record.kind, `${path}.kind`);
  if (kind === 'joint_state') {
    exactKeysAt(
      record,
      [
        'api_version',
        'joint_names',
        'kind',
        'positions',
        'source_stamp_ns',
        'topic',
      ],
      path,
    );
    const names = boundedArrayAt(record.joint_names, `${path}.joint_names`, 4096)
      .map((item, index) => nonEmptyStringAt(item, `${path}.joint_names[${index}]`));
    const positions = boundedArrayAt(record.positions, `${path}.positions`, 4096)
      .map((item, index) => numberAt(item, `${path}.positions[${index}]`));
    if (
      names.length === 0
      || names.length !== positions.length
      || new Set(names).size !== names.length
    ) {
      return fail(
        path,
        'equal non-empty unique joint_names and finite positions arrays',
      );
    }
    const topic = nonEmptyStringAt(record.topic, `${path}.topic`);
    if (!topic.startsWith('/') || topic.includes('//') || /\s/u.test(topic)) {
      return fail(`${path}.topic`, 'one absolute ROS topic');
    }
    return {
      api_version: 'manyforge.ros_pose_observation.v2',
      joint_names: names,
      kind,
      positions,
      source_stamp_ns: record.source_stamp_ns === null ? null : parseNanoseconds(
        record.source_stamp_ns,
        `${path}.source_stamp_ns`,
      ),
      topic,
    };
  }
  if (kind !== 'simulation_entity_pose' && kind !== 'detection3d_pose') {
    return fail(`${path}.kind`, 'a supported ROS pose-observation kind');
  }
  exactKeysAt(record, ['api_version', 'kind', 'pose', 'source'], path);
  const pose = parseSceneRuntimeObservationPose(record.pose, `${path}.pose`);
  const source = objectAt(record.source, `${path}.source`);
  if (kind === 'simulation_entity_pose') {
    exactKeysAt(source, ['entity'], `${path}.source`);
    return {
      api_version: 'manyforge.ros_pose_observation.v2',
      kind,
      pose,
      source: {
        entity: nonEmptyStringAt(source.entity, `${path}.source.entity`),
      },
    };
  }
  exactKeysAt(
    source,
    [
      'class_id',
      'confidence',
      'detection_id',
      'max_position_variance_m2',
      'topic',
    ],
    `${path}.source`,
  );
  const confidence = numberAt(source.confidence, `${path}.source.confidence`);
  const variance = numberAt(
    source.max_position_variance_m2,
    `${path}.source.max_position_variance_m2`,
  );
  if (variance < 0) {
    return fail(`${path}.source`, 'non-negative position covariance');
  }
  return {
    api_version: 'manyforge.ros_pose_observation.v2',
    kind,
    pose,
    source: {
      class_id: stringAt(source.class_id, `${path}.source.class_id`),
      confidence,
      detection_id: stringAt(
        source.detection_id,
        `${path}.source.detection_id`,
      ),
      max_position_variance_m2: variance,
      topic: nonEmptyStringAt(source.topic, `${path}.source.topic`),
    },
  };
}

function parseSceneRuntimeUnknownPayload(
  value: unknown,
  path: string,
): SceneRuntimeUnknownObservationPayload {
  const record = objectAt(value, path);
  exactKeysAt(record, ['api_version', 'kind', 'source_error'], path);
  if (record.api_version !== 'manyforge.ros_pose_observation.v2') {
    return fail(`${path}.api_version`, 'manyforge.ros_pose_observation.v2');
  }
  const kind = stringAt(record.kind, `${path}.kind`);
  if (![
    'joint_state',
    'simulation_entity_pose',
    'detection3d_pose',
  ].includes(kind)) {
    return fail(`${path}.kind`, 'a supported ROS pose-observation kind');
  }
  const sourceError = objectAt(record.source_error, `${path}.source_error`);
  exactKeysAt(
    sourceError,
    ['code', 'detail', 'source_key'],
    `${path}.source_error`,
  );
  return {
    api_version: 'manyforge.ros_pose_observation.v2',
    kind: kind as SceneRuntimeUnknownObservationPayload['kind'],
    source_error: {
      code: nonEmptyStringAt(sourceError.code, `${path}.source_error.code`),
      detail: nonEmptyStringAt(
        sourceError.detail,
        `${path}.source_error.detail`,
      ),
      source_key: nonEmptyStringAt(
        sourceError.source_key,
        `${path}.source_error.source_key`,
      ),
    },
  };
}

function parseSceneRuntimePoseSource(
  value: unknown,
  path: string,
): SceneRuntimePoseSource {
  const record = objectAt(value, path);
  exactKeysAt(
    record,
    [
      'buffered',
      'eligible',
      'expires_at_ns',
      'fresh',
      'object_id',
      'observed_at_ns',
      'payload',
      'policy_id',
      'quality',
      'source_epoch',
      'source_epoch_order',
      'source_id',
      'source_sequence',
    ],
    path,
  );
  const observedAt = monotonicNanosecondsAt(record.observed_at_ns, `${path}.observed_at_ns`);
  const expiresAt = monotonicNanosecondsAt(record.expires_at_ns, `${path}.expires_at_ns`);
  if (BigInt(expiresAt) < BigInt(observedAt)) {
    return fail(`${path}.expires_at_ns`, 'at least observed_at_ns');
  }
  const base = {
    buffered: booleanAt(record.buffered, `${path}.buffered`),
    expires_at_ns: expiresAt,
    object_id: nonEmptyStringAt(record.object_id, `${path}.object_id`),
    observed_at_ns: observedAt,
    policy_id: nonEmptyStringAt(record.policy_id, `${path}.policy_id`),
    source_epoch: nonEmptyStringAt(record.source_epoch, `${path}.source_epoch`),
    source_epoch_order: positiveIntegerAt(
      record.source_epoch_order,
      `${path}.source_epoch_order`,
    ),
    source_id: nonEmptyStringAt(record.source_id, `${path}.source_id`),
    source_sequence: positiveIntegerAt(
      record.source_sequence,
      `${path}.source_sequence`,
    ),
  };
  const quality = stringAt(record.quality, `${path}.quality`);
  const eligible = booleanAt(record.eligible, `${path}.eligible`);
  const fresh = booleanAt(record.fresh, `${path}.fresh`);
  if (quality === 'unknown') {
    if (base.buffered || eligible || fresh) {
      return fail(path, 'unknown quality to be unbuffered, ineligible, and stale');
    }
    return {
      ...base,
      buffered: false,
      eligible: false,
      fresh: false,
      payload: parseSceneRuntimeUnknownPayload(record.payload, `${path}.payload`),
      quality,
    };
  }
  if (quality !== 'good') {
    return fail(`${path}.quality`, '"good" or "unknown"');
  }
  if (base.buffered && (!eligible || !fresh)) {
    return fail(path, 'a buffered observation to be eligible and fresh');
  }
  return {
    ...base,
    eligible,
    fresh,
    payload: parseSceneRuntimeGoodPayload(record.payload, `${path}.payload`),
    quality,
  };
}

export function parseSceneTransactionResponse(value: unknown): SceneTransactionResponse {
  const record = objectAt(value, 'sceneTransaction');
  exactKeysAt(
    record,
    [
      'api_version',
      'committed_projection_revision',
      'error',
      'ok',
      'projection_status',
      'replay_status',
      'request_id',
      'request_owner_epoch',
      'transaction_sequence',
      'update_status',
    ],
    'sceneTransaction',
  );
  const apiVersion = stringAt(record.api_version, 'sceneTransaction.api_version');
  if (apiVersion !== 'manyforge.manager_command_response.v1') {
    return fail('sceneTransaction.api_version', 'manyforge.manager_command_response.v1');
  }
  if (record.ok !== true || record.error !== null) {
    return fail('sceneTransaction', 'a successful committed response');
  }
  const committed = parseProjectionRevision(
    record.committed_projection_revision,
    'sceneTransaction.committed_projection_revision',
  );
  const requestOwnerEpoch = stringAt(
    record.request_owner_epoch,
    'sceneTransaction.request_owner_epoch',
  );
  return {
    api_version: apiVersion,
    committed_projection_revision: committed,
    error: null,
    ok: true,
    projection_status: stringAt(record.projection_status, 'sceneTransaction.projection_status'),
    replay_status: stringAt(record.replay_status, 'sceneTransaction.replay_status'),
    request_id: stringAt(record.request_id, 'sceneTransaction.request_id'),
    request_owner_epoch: requestOwnerEpoch,
    transaction_sequence: positiveIntegerAt(
      record.transaction_sequence,
      'sceneTransaction.transaction_sequence',
    ),
    update_status: stringAt(record.update_status, 'sceneTransaction.update_status'),
  };
}

export function parseDraftMutationResponse(value: unknown): DraftMutationResponse {
  const record = objectAt(value, 'draftMutation');
  exactKeysAt(record, ['mutation', 'details', 'workspace'], 'draftMutation');
  return {
    mutation: stringAt(record.mutation, 'draftMutation.mutation'),
    details: jsonRecordAt(record.details, 'draftMutation.details'),
    workspace: parseProgramWorkspaceResponse(record.workspace),
  };
}

function parseFileBrowserEntry(value: unknown, path: string): FileBrowserEntry {
  const record = objectAt(value, path);
  const entryType = stringAt(record.entryType, `${path}.entryType`);
  if (entryType !== 'directory' && entryType !== 'file') {
    return fail(`${path}.entryType`, '"directory" or "file"');
  }
  return {
    name: stringAt(record.name, `${path}.name`),
    path: stringAt(record.path, `${path}.path`),
    entryType,
    selectable: booleanAt(record.selectable, `${path}.selectable`),
  };
}

export function parseFileBrowserResponse(value: unknown): FileBrowserResponse {
  const record = objectAt(value, 'files');
  return {
    currentPath: stringAt(record.currentPath, 'files.currentPath'),
    parentPath: nullableStringAt(record.parentPath, 'files.parentPath'),
    roots: stringArrayAt(record.roots, 'files.roots'),
    entries: arrayAt(record.entries, 'files.entries').map((entry, index) =>
      parseFileBrowserEntry(entry, `files.entries[${index}]`),
    ),
  };
}

function parsePrimitive(value: unknown, path: string): SceneUrdfPrimitive | null {
  if (value === null) {
    return null;
  }
  const record = objectAt(value, path);
  const type = stringAt(record.type, `${path}.type`);
  if (type === 'box') {
    return { type, size_m: vector3At(record.size_m, `${path}.size_m`) };
  }
  if (type === 'cylinder') {
    return {
      type,
      radius_m: numberAt(record.radius_m, `${path}.radius_m`),
      length_m: numberAt(record.length_m, `${path}.length_m`),
    };
  }
  if (type === 'sphere') {
    return { type, radius_m: numberAt(record.radius_m, `${path}.radius_m`) };
  }
  return fail(`${path}.type`, '"box", "cylinder", or "sphere"');
}

function parseOrigin(value: unknown, path: string): SceneUrdfOrigin {
  const record = objectAt(value, path);
  return {
    position_m: vector3At(record.position_m, `${path}.position_m`),
    orientation_rpy_rad: vector3At(
      record.orientation_rpy_rad,
      `${path}.orientation_rpy_rad`,
    ),
    orientation_xyzw: quaternionAt(
      record.orientation_xyzw,
      `${path}.orientation_xyzw`,
    ),
  };
}

function parseVisual(value: unknown, path: string): SceneUrdfVisual {
  const record = objectAt(value, path);
  const visual: SceneUrdfVisual = {
    index: numberAt(record.index, `${path}.index`),
    mesh_url: nullableStringAt(record.mesh_url, `${path}.mesh_url`),
    primitive: parsePrimitive(record.primitive, `${path}.primitive`),
    visual_origin: parseOrigin(record.visual_origin, `${path}.visual_origin`),
  };
  if ((visual.mesh_url === null) === (visual.primitive === null)) {
    return fail(path, 'to declare exactly one mesh_url or primitive');
  }
  return visual;
}

function parseCollision(value: unknown, path: string): SceneUrdfCollision {
  const record = objectAt(value, path);
  const collision: SceneUrdfCollision = {
    index: numberAt(record.index, `${path}.index`),
    mesh_url: nullableStringAt(record.mesh_url, `${path}.mesh_url`),
    primitive: parsePrimitive(record.primitive, `${path}.primitive`),
    collision_origin: parseOrigin(
      record.collision_origin,
      `${path}.collision_origin`,
    ),
  };
  if ((collision.mesh_url === null) === (collision.primitive === null)) {
    return fail(path, 'to declare exactly one mesh_url or primitive');
  }
  return collision;
}

function parseLink(value: unknown, path: string): SceneUrdfLink {
  const record = objectAt(value, path);
  const neutral = objectAt(record.neutral_transform, `${path}.neutral_transform`);
  return {
    name: stringAt(record.name, `${path}.name`),
    neutral_transform: {
      relative_to: stringAt(
        neutral.relative_to,
        `${path}.neutral_transform.relative_to`,
      ),
      position_m: vector3At(
        neutral.position_m,
        `${path}.neutral_transform.position_m`,
      ),
      orientation_xyzw: quaternionAt(
        neutral.orientation_xyzw,
        `${path}.neutral_transform.orientation_xyzw`,
      ),
    },
    visuals: arrayAt(record.visuals, `${path}.visuals`).map((visual, index) =>
      parseVisual(visual, `${path}.visuals[${index}]`),
    ),
    collisions: arrayAt(record.collisions, `${path}.collisions`).map(
      (collision, index) =>
        parseCollision(collision, `${path}.collisions[${index}]`),
    ),
  };
}

function parseJoint(value: unknown, path: string): SceneUrdfJoint {
  const record = objectAt(value, path);
  const originRecord = objectAt(record.origin, `${path}.origin`);
  const origin = parseOrigin(originRecord, `${path}.origin`);
  const rawUnit = record.coordinate_unit;
  if (rawUnit !== null && rawUnit !== 'rad' && rawUnit !== 'm') {
    return fail(`${path}.coordinate_unit`, '"rad", "m", or null');
  }
  const rawMimic = record.mimic;
  const mimic = rawMimic === null ? null : objectAt(rawMimic, `${path}.mimic`);
  return {
    name: stringAt(record.name, `${path}.name`),
    type: stringAt(record.type, `${path}.type`),
    axis: vector3At(record.axis, `${path}.axis`),
    parent_link: stringAt(record.parent_link, `${path}.parent_link`),
    child_link: stringAt(record.child_link, `${path}.child_link`),
    origin: {
      relative_to: stringAt(
        originRecord.relative_to,
        `${path}.origin.relative_to`,
      ),
      ...origin,
    },
    coordinate_unit: rawUnit,
    neutral_position: nullableNumberAt(
      record.neutral_position,
      `${path}.neutral_position`,
    ),
    limit_lower: nullableNumberAt(record.limit_lower, `${path}.limit_lower`),
    limit_upper: nullableNumberAt(record.limit_upper, `${path}.limit_upper`),
    effort_limit: nullableNumberAt(record.effort_limit, `${path}.effort_limit`),
    velocity_limit: nullableNumberAt(
      record.velocity_limit,
      `${path}.velocity_limit`,
    ),
    mimic: mimic === null
      ? null
      : {
          master: stringAt(mimic.master, `${path}.mimic.master`),
          multiplier: numberAt(mimic.multiplier, `${path}.mimic.multiplier`),
          offset: numberAt(mimic.offset, `${path}.mimic.offset`),
        },
    is_mimic_master: booleanAt(
      record.is_mimic_master,
      `${path}.is_mimic_master`,
    ),
  };
}

function literalAt<T extends string | boolean>(
  value: unknown,
  expected: T,
  path: string,
): T {
  if (value !== expected) {
    return fail(path, `the literal ${JSON.stringify(expected)}`);
  }
  return expected;
}

function parseProjection(value: unknown): RetainedNeutralProjection {
  const record = objectAt(value, 'manifest.projection');
  return {
    kind: literalAt(record.kind, 'retained_neutral', 'manifest.projection.kind'),
    source: literalAt(
      record.source,
      'active_retained_viewer_model',
      'manifest.projection.source',
    ),
    root_frame: stringAt(record.root_frame, 'manifest.projection.root_frame'),
    position_unit: literalAt(
      record.position_unit,
      'm',
      'manifest.projection.position_unit',
    ),
    angle_unit: literalAt(
      record.angle_unit,
      'rad',
      'manifest.projection.angle_unit',
    ),
    quaternion_order: literalAt(
      record.quaternion_order,
      'xyzw',
      'manifest.projection.quaternion_order',
    ),
    joint_coordinate_policy: literalAt(
      record.joint_coordinate_policy,
      'zero_independent_with_retained_mimic',
      'manifest.projection.joint_coordinate_policy',
    ),
    operational_validity: literalAt(
      record.operational_validity,
      'not_asserted',
      'manifest.projection.operational_validity',
    ),
    runtime_state_used: literalAt(
      record.runtime_state_used,
      false,
      'manifest.projection.runtime_state_used',
    ),
  };
}

export function parseSceneUrdfManifest(value: unknown): SceneUrdfManifest {
  const record = objectAt(value, 'manifest');
  if ('authority' in record || 'link_poses' in record || 'runtime_state' in record) {
    return fail(
      'manifest',
      'free of runtime authority and live-state fields',
    );
  }
  const rootLink = stringAt(record.root_link, 'manifest.root_link');
  const projection = parseProjection(record.projection);
  if (projection.root_frame !== rootLink) {
    return fail('manifest.projection.root_frame', 'equal to manifest.root_link');
  }
  const links = arrayAt(record.links, 'manifest.links').map((link, index) =>
    parseLink(link, `manifest.links[${index}]`),
  );
  if (links.some((link) => link.neutral_transform.relative_to !== rootLink)) {
    return fail(
      'manifest.links[].neutral_transform.relative_to',
      'equal to manifest.root_link',
    );
  }
  const linkNames = new Set(links.map((link) => link.name));
  if (linkNames.size !== links.length) {
    return fail('manifest.links[].name', 'unique');
  }
  if (!linkNames.has(rootLink)) {
    return fail('manifest.links', 'to include manifest.root_link');
  }

  return {
    cell_id: stringAt(record.cell_id, 'manifest.cell_id'),
    graph_revision: stringAt(record.graph_revision, 'manifest.graph_revision'),
    retained_bundle_id: stringAt(
      record.retained_bundle_id,
      'manifest.retained_bundle_id',
    ),
    viewer_model_revision: stringAt(
      record.viewer_model_revision,
      'manifest.viewer_model_revision',
    ),
    model_key: stringAt(record.model_key, 'manifest.model_key'),
    root_link: rootLink,
    projection,
    links,
    joints: arrayAt(record.joints, 'manifest.joints').map((joint, index) =>
      parseJoint(joint, `manifest.joints[${index}]`),
    ),
  };
}

function sourceFreshnessAt(value: unknown, path: string): SourceFreshness {
  const freshness = stringAt(value, path);
  if (!['fresh', 'stale', 'unavailable'].includes(freshness)) {
    return fail(path, 'fresh, stale, or unavailable');
  }
  return freshness as SourceFreshness;
}

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
  return {
    schemaVersion,
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
    'components', 'basis',
  ], path);
  const schemaVersion = stringAt(record.schema_version, `${path}.schema_version`);
  if (schemaVersion !== 'manyforge.component_overview.v1') {
    return fail(`${path}.schema_version`, 'manyforge.component_overview.v1');
  }
  const readiness = stringAt(record.readiness, `${path}.readiness`);
  if (!['ready', 'waiting', 'degraded', 'fault', 'offline'].includes(readiness)) {
    return fail(`${path}.readiness`, 'a supported Component Overview readiness');
  }
  const componentStatuses = new Set([
    'healthy', 'waiting', 'warning', 'fault', 'offline',
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
    'execution_targets', 'execution_control_state', 'program_cycle',
    'draft_identity', 'receiver_epoch',
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
  const draft = objectAt(record.draft_identity, 'headlessState.draft_identity');
  exactKeysAt(draft, ['draft_id', 'draft_revision'], 'headlessState.draft_identity');
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
    executionControlStatus: record.execution_targets === null
      && record.execution_control_state === null
      ? null
      : parseExecutionControlStatus(
          record.execution_targets,
          record.execution_control_state,
        ),
    programCycle: parseProgramCycleStatus(record.program_cycle),
    draftIdentity: {
      draftId: stringAt(draft.draft_id, 'headlessState.draft_identity.draft_id'),
      draftRevision: integerAt(draft.draft_revision, 'headlessState.draft_identity.draft_revision'),
    },
    receiverEpoch: stringAt(record.receiver_epoch, 'headlessState.receiver_epoch'),
    snapshotSequence: positiveIntegerAt(record.snapshot_sequence, 'headlessState.snapshot_sequence'),
  };
}

export function parseObjectResponse(value: unknown): Record<string, unknown> {
  return objectAt(value, 'response');
}

function parseHistorianEvent(value: unknown, path: string): HistorianEvent {
  const record = objectAt(value, path);
  const schemaVersion = stringAt(record.schema_version, `${path}.schema_version`);
  if (schemaVersion !== 'manyforge.historian_event.v2') {
    return fail(`${path}.schema_version`, 'manyforge.historian_event.v2');
  }
  return {
    schemaVersion,
    eventId: stringAt(record.event_id, `${path}.event_id`),
    component: stringAt(record.component, `${path}.component`),
    eventType: stringAt(record.event_type, `${path}.event_type`),
    severity: stringAt(record.severity, `${path}.severity`) as 'error',
    occurredAtNs: nonnegativeInt64TextAt(
      record.occurred_at_ns,
      `${path}.occurred_at_ns`,
    ),
    occurredAtWall: stringAt(
      record.occurred_at_wall,
      `${path}.occurred_at_wall`,
    ),
    payload: objectAt(record.payload, `${path}.payload`),
  };
}

function parseHistorianEventSummary(
  value: unknown,
  path: string,
): HistorianEventSummary {
  const record = objectAt(value, path);
  exactKeysAt(record, [
    'component',
    'condition_id',
    'event_id',
    'event_type',
    'message',
    'occurred_at_ns',
    'occurred_at_wall',
    'operation_id',
    'program_run_id',
    'schema_version',
    'severity',
    'condition_severity',
    'occurred_at_utc_ns',
    'clock_owner',
    'wall_clock_regressed',
  ], path);
  const schemaVersion = stringAt(record.schema_version, `${path}.schema_version`);
  if (schemaVersion !== 'manyforge.historian_event_summary.v2') {
    return fail(`${path}.schema_version`, 'manyforge.historian_event_summary.v2');
  }
  return {
    schemaVersion,
    eventId: stringAt(record.event_id, `${path}.event_id`),
    component: stringAt(record.component, `${path}.component`),
    eventType: stringAt(record.event_type, `${path}.event_type`),
    severity: stringAt(record.severity, `${path}.severity`),
    occurredAtNs: nonnegativeInt64TextAt(
      record.occurred_at_ns,
      `${path}.occurred_at_ns`,
    ),
    occurredAtWall: stringAt(record.occurred_at_wall, `${path}.occurred_at_wall`),
    message: stringAt(record.message, `${path}.message`),
    programRunId: nullableStringAt(record.program_run_id, `${path}.program_run_id`),
    operationId: nullableStringAt(record.operation_id, `${path}.operation_id`),
    conditionId: nullableStringAt(record.condition_id, `${path}.condition_id`),
    conditionSeverity: stringAt(record.condition_severity, `${path}.condition_severity`),
    occurredAtUtcNs: record.occurred_at_utc_ns === null ? null : nonnegativeInt64TextAt(record.occurred_at_utc_ns, `${path}.occurred_at_utc_ns`),
    clockOwner: nullableStringAt(record.clock_owner, `${path}.clock_owner`),
    wallClockRegressed: booleanAt(record.wall_clock_regressed, `${path}.wall_clock_regressed`),
  };
}

function parseHistorianRetention(value: unknown): HistorianRetention | null {
  if (value === null) return null;
  const path = 'history.retention';
  const record = objectAt(value, path);
  exactKeysAt(record, ['schema_version', 'partitions', 'partition_row_limit', 'partition_record_byte_limit', 'database_page_byte_limit', 'duration_guaranteed', 'prior_pruning_known'], path);
  if (record.schema_version !== 'manyforge.historian_retention.v1' || record.duration_guaranteed !== false) {
    return fail(path, 'the bounded local retention v1 profile');
  }
  const partitions = arrayAt(record.partitions, `${path}.partitions`);
  if (partitions.length !== 2) return fail(path, 'routine and important retention partitions');
  return {
    schemaVersion: record.schema_version,
    partitionRowLimit: nonnegativeInt64TextAt(record.partition_row_limit, `${path}.partition_row_limit`),
    partitionRecordByteLimit: nonnegativeInt64TextAt(record.partition_record_byte_limit, `${path}.partition_record_byte_limit`),
    databasePageByteLimit: nonnegativeInt64TextAt(record.database_page_byte_limit, `${path}.database_page_byte_limit`),
    durationGuaranteed: false,
    priorPruningKnown: booleanAt(record.prior_pruning_known, `${path}.prior_pruning_known`),
    partitions: partitions.map((value, index) => {
      const partPath = `${path}.partitions[${index}]`;
      const part = objectAt(value, partPath);
      exactKeysAt(part, ['partition', 'retained_count', 'retained_bytes', 'pruned_count', 'pruned_bytes', 'last_pruned_sequence', 'unknown_time_count', 'regressed_time_count', 'loss_counter_saturated', 'first_recorded_utc_ns', 'last_recorded_utc_ns'], partPath);
      const partition = index === 0 ? 'routine' : 'important';
      if (part.partition !== partition) return fail(partPath, partition);
      const scalar = (key: string) => nonnegativeInt64TextAt(part[key], `${partPath}.${key}`);
      const nullable = (key: string) => part[key] === null ? null : scalar(key);
      return {
        partition, retainedCount: scalar('retained_count'), retainedBytes: scalar('retained_bytes'),
        prunedCount: scalar('pruned_count'), prunedBytes: scalar('pruned_bytes'),
        lastPrunedSequence: nullable('last_pruned_sequence'), unknownTimeCount: scalar('unknown_time_count'),
        regressedTimeCount: scalar('regressed_time_count'),
        lossCounterSaturated: booleanAt(part.loss_counter_saturated, `${partPath}.loss_counter_saturated`),
        firstRecordedUtcNs: nullable('first_recorded_utc_ns'), lastRecordedUtcNs: nullable('last_recorded_utc_ns'),
      };
    }),
  };
}

export function parseHistorianDiscovery(value: unknown): HistorianDiscovery {
  const record = objectAt(value, 'history.discovery');
  exactKeysAt(record, ['schema_version', 'kind', 'values', 'truncated', 'next_after'], 'history.discovery');
  if (record.schema_version !== 'manyforge.historian_discovery.v1') return fail('history.discovery.schema_version', 'manyforge.historian_discovery.v1');
  const kind = stringAt(record.kind, 'history.discovery.kind');
  if (kind !== 'program_run_id' && kind !== 'component' && kind !== 'channel' && kind !== 'event_type') return fail('history.discovery.kind', 'a declared discovery kind');
  const values = arrayAt(record.values, 'history.discovery.values').map((value) => stringAt(value, 'history.discovery.value'));
  if (values.length > 32 || new Set(values).size !== values.length || values.some((value) => !value || new TextEncoder().encode(value).length > 1024 || value !== value.normalize('NFC'))) return fail('history.discovery.values', 'at most 32 unique nonempty NFC values of at most 1024 bytes');
  const truncated = booleanAt(record.truncated, 'history.discovery.truncated');
  const nextAfter = record.next_after === null ? null : stringAt(record.next_after, 'history.discovery.next_after');
  if ((truncated && values.length === 0) || nextAfter !== (truncated ? values.at(-1) : null)) return fail('history.discovery.next_after', 'an advancing exact cursor');
  return { schemaVersion: record.schema_version, kind, values, truncated, nextAfter };
}

export function parseHistorianPage(value: unknown): HistorianPage {
  const record = objectAt(value, 'history');
  exactKeysAt(record, [
    'events',
    'next_before_sequence',
    'schema_version',
    'truncated',
    'time_window',
    'retention',
  ], 'history');
  const schemaVersion = stringAt(record.schema_version, 'history.schema_version');
  if (schemaVersion !== 'manyforge.historian_summary_page.v2') {
    return fail('history.schema_version', 'manyforge.historian_summary_page.v2');
  }
  const window = objectAt(record.time_window, 'history.time_window');
  exactKeysAt(window, ['start_utc_ns', 'end_utc_ns'], 'history.time_window');
  const events = arrayAt(record.events, 'history.events');
  if (events.length > 32) return fail('history.events', 'at most 32 summary rows');
  return {
    schemaVersion,
    events: events.map((value, index) => {
      const stored = objectAt(value, `history.events[${index}]`);
      exactKeysAt(stored, ['event', 'sequence'], `history.events[${index}]`);
      return {
        sequence: positiveIntegerAt(
          stored.sequence,
          `history.events[${index}].sequence`,
        ),
        event: parseHistorianEventSummary(
          stored.event,
          `history.events[${index}].event`,
        ),
      };
    }),
    nextBeforeSequence: record.next_before_sequence === null
      ? null
      : positiveIntegerAt(
          record.next_before_sequence,
          'history.next_before_sequence',
        ),
    truncated: booleanAt(record.truncated, 'history.truncated'),
    timeWindow: {
      startUtcNs: window.start_utc_ns === null ? null : nonnegativeInt64TextAt(window.start_utc_ns, 'history.time_window.start_utc_ns'),
      endUtcNs: window.end_utc_ns === null ? null : nonnegativeInt64TextAt(window.end_utc_ns, 'history.time_window.end_utc_ns'),
    },
    retention: parseHistorianRetention(record.retention),
  };
}

function historianDetailSectionAt(
  value: unknown,
  path: string,
): HistorianDetail['sections']['incident'] {
  const record = objectAt(value, path);
  exactKeysAt(record, ['available', 'reason', 'value'], path);
  const available = booleanAt(record.available, `${path}.available`);
  const reason = record.reason === null ? null : stringAt(record.reason, `${path}.reason`);
  if (available === (reason !== null) || available === (record.value === null)) {
    return fail(path, 'one consistent available/reason/value union');
  }
  return { available, reason, value: record.value };
}

export function parseHistorianDetail(value: unknown): HistorianDetail {
  const record = objectAt(value, 'historyDetail');
  exactKeysAt(record, ['event', 'schema_version', 'sections', 'sequence'], 'historyDetail');
  const schemaVersion = stringAt(record.schema_version, 'historyDetail.schema_version');
  if (schemaVersion !== 'manyforge.historian_detail.v2') {
    return fail('historyDetail.schema_version', 'manyforge.historian_detail.v2');
  }
  const sections = objectAt(record.sections, 'historyDetail.sections');
  exactKeysAt(
    sections,
    ['blackboard', 'execution_digest', 'incident', 'latency'],
    'historyDetail.sections',
  );
  return {
    schemaVersion,
    sequence: positiveIntegerAt(record.sequence, 'historyDetail.sequence'),
    event: parseHistorianEvent(record.event, 'historyDetail.event'),
    sections: {
      incident: historianDetailSectionAt(sections.incident, 'historyDetail.sections.incident'),
      executionDigest: historianDetailSectionAt(
        sections.execution_digest,
        'historyDetail.sections.execution_digest',
      ),
      blackboard: historianDetailSectionAt(
        sections.blackboard,
        'historyDetail.sections.blackboard',
      ),
      latency: historianDetailSectionAt(sections.latency, 'historyDetail.sections.latency'),
    },
  };
}

function sceneRevisionAt(value: unknown, path: string): SceneRevisionValue {
  const record = objectAt(value, path);
  const revisionId = integerAt(record.revision_id, `${path}.revision_id`);
  if (revisionId < 1) return fail(`${path}.revision_id`, 'positive');
  return {
    scene_id: stringAt(record.scene_id, `${path}.scene_id`),
    revision_id: revisionId,
    content_hash: stringAt(record.content_hash, `${path}.content_hash`),
  };
}

function directControlJointAt(
  value: unknown,
  path: string,
): DirectControlJoint {
  const record = objectAt(value, path);
  exactKeysAt(record, [
    'max_acceleration_rad_s2',
    'max_position_rad',
    'max_velocity_rad_s',
    'min_position_rad',
    'source_joint_name',
  ], path);
  return {
    sourceJointName: stringAt(
      record.source_joint_name,
      `${path}.source_joint_name`,
    ),
    minPositionRad: numberAt(
      record.min_position_rad,
      `${path}.min_position_rad`,
    ),
    maxPositionRad: numberAt(
      record.max_position_rad,
      `${path}.max_position_rad`,
    ),
    maxVelocityRadS: numberAt(
      record.max_velocity_rad_s,
      `${path}.max_velocity_rad_s`,
    ),
    maxAccelerationRadS2: nullableNumberAt(
      record.max_acceleration_rad_s2,
      `${path}.max_acceleration_rad_s2`,
    ),
  };
}

function manualControlContractsAt(
  value: unknown,
  path: string,
): ManualControlContracts {
  const record = objectAt(value, path);
  exactKeysAt(record, [
    'command_schema', 'control_path',
    'operation_capability_definition_revision', 'operation_capability_id',
    'supporting_providers',
  ], path);
  const commandSchema = record.command_schema === null ? null : (() => {
    const schema = objectAt(record.command_schema, `${path}.command_schema`);
    exactKeysAt(schema, ['command_schema_id', 'command_schema_revision'], `${path}.command_schema`);
    return {
      commandSchemaId: stringAt(schema.command_schema_id, `${path}.command_schema.command_schema_id`),
      commandSchemaRevision: digestAt(schema.command_schema_revision, `${path}.command_schema.command_schema_revision`),
    };
  })();
  const controlPath = record.control_path === null ? null : (() => {
    const control = objectAt(record.control_path, `${path}.control_path`);
    exactKeysAt(control, [
      'control_binding_id', 'control_binding_revision',
      'control_interface_contract_id', 'control_interface_revision',
      'controller_topology_id', 'stop_evidence_policy_id',
      'stop_evidence_policy_revision', 'topology_revision',
    ], `${path}.control_path`);
    return {
      controlBindingId: stringAt(control.control_binding_id, `${path}.control_path.control_binding_id`),
      controlBindingRevision: digestAt(control.control_binding_revision, `${path}.control_path.control_binding_revision`),
      controlInterfaceContractId: stringAt(control.control_interface_contract_id, `${path}.control_path.control_interface_contract_id`),
      controlInterfaceRevision: digestAt(control.control_interface_revision, `${path}.control_path.control_interface_revision`),
      controllerTopologyId: stringAt(control.controller_topology_id, `${path}.control_path.controller_topology_id`),
      topologyRevision: digestAt(control.topology_revision, `${path}.control_path.topology_revision`),
      stopEvidencePolicyId: stringAt(control.stop_evidence_policy_id, `${path}.control_path.stop_evidence_policy_id`),
      stopEvidencePolicyRevision: digestAt(control.stop_evidence_policy_revision, `${path}.control_path.stop_evidence_policy_revision`),
    };
  })();
  const supportingProviders = boundedArrayAt(
    record.supporting_providers,
    `${path}.supporting_providers`,
    64,
  ).map((item, index) => {
    const itemPath = `${path}.supporting_providers[${index}]`;
    const provider = objectAt(item, itemPath);
    exactKeysAt(provider, [
      'capability_definition_revision', 'capability_id',
      'capability_provider_binding_id', 'capability_provider_binding_revision',
      'provider_manifest_id', 'provider_manifest_revision',
    ], itemPath);
    return {
      capabilityId: stringAt(provider.capability_id, `${itemPath}.capability_id`),
      capabilityDefinitionRevision: digestAt(provider.capability_definition_revision, `${itemPath}.capability_definition_revision`),
      capabilityProviderBindingId: stringAt(provider.capability_provider_binding_id, `${itemPath}.capability_provider_binding_id`),
      capabilityProviderBindingRevision: digestAt(provider.capability_provider_binding_revision, `${itemPath}.capability_provider_binding_revision`),
      providerManifestId: stringAt(provider.provider_manifest_id, `${itemPath}.provider_manifest_id`),
      providerManifestRevision: digestAt(provider.provider_manifest_revision, `${itemPath}.provider_manifest_revision`),
    };
  });
  if (new Set(supportingProviders.map(
    (provider) => provider.capabilityProviderBindingId,
  )).size !== supportingProviders.length) {
    return fail(`${path}.supporting_providers`, 'unique provider bindings');
  }
  return {
    operationCapabilityId: stringAt(record.operation_capability_id, `${path}.operation_capability_id`),
    operationCapabilityDefinitionRevision: digestAt(record.operation_capability_definition_revision, `${path}.operation_capability_definition_revision`),
    commandSchema,
    controlPath,
    supportingProviders,
  };
}

function manualControlMotionDescriptorAt(
  value: unknown,
  path: string,
): ManualControlMotionDescriptor {
  const record = objectAt(value, path);
  exactKeysAt(record, [
    'command_target_id', 'component_role_id', 'contracts',
    'controllable_component_id', 'frames', 'joints', 'operational_joint_limits',
    'operational_limit', 'ordinary_jerk', 'schema_version', 'target_role_id',
  ], path);
  const schemaVersion = stringAt(record.schema_version, `${path}.schema_version`);
  if (schemaVersion !== 'manyforge.manual_control_motion_descriptor.v4') {
    return fail(`${path}.schema_version`, 'the supported motion descriptor');
  }
  const operationalLimit = record.operational_limit === null
    ? null
    : (() => {
        const limitPath = `${path}.operational_limit`;
        const limit = objectAt(record.operational_limit, limitPath);
        exactKeysAt(limit, [
          'catalog_policy_maximum_tcp_linear_speed_mps',
          'deployment_maximum_tcp_linear_speed_mps',
          'effective_maximum_tcp_linear_speed_mps', 'policy_id',
          'policy_revision', 'selected_scope', 'source_content_digest',
        ], limitPath);
        const selectedScope = stringAt(limit.selected_scope, `${limitPath}.selected_scope`);
        if (selectedScope !== 'cell' && selectedScope !== 'command_target') {
          return fail(`${limitPath}.selected_scope`, 'cell or command_target');
        }
        return {
          catalogPolicyMaximumTcpLinearSpeedMps: positiveNumberAt(
            limit.catalog_policy_maximum_tcp_linear_speed_mps,
            `${limitPath}.catalog_policy_maximum_tcp_linear_speed_mps`,
          ),
          deploymentMaximumTcpLinearSpeedMps:
            limit.deployment_maximum_tcp_linear_speed_mps === null
              ? null
              : positiveNumberAt(
                  limit.deployment_maximum_tcp_linear_speed_mps,
                  `${limitPath}.deployment_maximum_tcp_linear_speed_mps`,
                ),
          effectiveMaximumTcpLinearSpeedMps: positiveNumberAt(
            limit.effective_maximum_tcp_linear_speed_mps,
            `${limitPath}.effective_maximum_tcp_linear_speed_mps`,
          ),
          policyId: stringAt(limit.policy_id, `${limitPath}.policy_id`),
          policyRevision: digestAt(limit.policy_revision, `${limitPath}.policy_revision`),
          selectedScope: selectedScope as 'cell' | 'command_target',
          sourceContentDigest: digestAt(
            limit.source_content_digest,
            `${limitPath}.source_content_digest`,
          ),
        };
      })();
  const frames = boundedArrayAt(record.frames, `${path}.frames`, 256).map((item, index) => {
    const itemPath = `${path}.frames[${index}]`;
    const frame = objectAt(item, itemPath);
    exactKeysAt(frame, ['compiled_frame_name', 'frame_role_id', 'model_instance_id', 'model_revision'], itemPath);
    return {
      compiledFrameName: stringAt(frame.compiled_frame_name, `${itemPath}.compiled_frame_name`),
      frameRoleId: stringAt(frame.frame_role_id, `${itemPath}.frame_role_id`),
      modelInstanceId: stringAt(frame.model_instance_id, `${itemPath}.model_instance_id`),
      modelRevision: digestAt(frame.model_revision, `${itemPath}.model_revision`),
    };
  });
  if (
    frames.length === 0
    || new Set(frames.map((frame) => frame.frameRoleId)).size !== frames.length
  ) {
    return fail(`${path}.frames`, 'non-empty unique frame roles');
  }
  const joints = boundedArrayAt(record.joints, `${path}.joints`, 1024).map((item, index) => {
    const itemPath = `${path}.joints[${index}]`;
    const joint = objectAt(item, itemPath);
    exactKeysAt(joint, [
      'compiled_joint_name', 'max_acceleration_rad_s2', 'max_position_rad',
      'max_velocity_rad_s', 'min_position_rad', 'source_joint_name', 'unit',
    ], itemPath);
    if (stringAt(joint.unit, `${itemPath}.unit`) !== 'rad') {
      return fail(`${itemPath}.unit`, 'rad');
    }
    return {
      compiledJointName: stringAt(joint.compiled_joint_name, `${itemPath}.compiled_joint_name`),
      sourceJointName: stringAt(joint.source_joint_name, `${itemPath}.source_joint_name`),
      minPositionRad: numberAt(joint.min_position_rad, `${itemPath}.min_position_rad`),
      maxPositionRad: numberAt(joint.max_position_rad, `${itemPath}.max_position_rad`),
      maxVelocityRadS: positiveNumberAt(joint.max_velocity_rad_s, `${itemPath}.max_velocity_rad_s`),
      maxAccelerationRadS2: nullableNumberAt(joint.max_acceleration_rad_s2, `${itemPath}.max_acceleration_rad_s2`),
      unit: 'rad' as const,
    };
  });
  if (
    joints.length === 0
    || new Set(joints.map((joint) => joint.sourceJointName)).size !== joints.length
    || new Set(joints.map((joint) => joint.compiledJointName)).size !== joints.length
  ) {
    return fail(`${path}.joints`, 'non-empty unique source and compiled joints');
  }
  for (const [index, joint] of joints.entries()) {
    if (
      joint.minPositionRad > joint.maxPositionRad
      || joint.maxVelocityRadS <= 0
      || (joint.maxAccelerationRadS2 !== null && joint.maxAccelerationRadS2 <= 0)
    ) {
      return fail(`${path}.joints[${index}]`, 'ordered finite limits');
    }
  }
  const ordinaryJerk: ManualControlMotionDescriptor['ordinaryJerk'] = record.ordinary_jerk === null
    ? null : (() => {
      const base = `${path}.ordinary_jerk`;
      const profile = objectAt(record.ordinary_jerk, base);
      exactKeysAt(profile, ['enabled', 'preset_reference_time_s', 'joint_limits'], base);
      const enabled = booleanAt(profile.enabled, `${base}.enabled`);
      const jointLimits = boundedArrayAt(profile.joint_limits, `${base}.joint_limits`, 1024)
        .map((value, index) => {
          const itemPath = `${base}.joint_limits[${index}]`;
          const row = objectAt(value, itemPath);
          exactKeysAt(row, ['source_joint_name', 'source', 'maximum_rad_s3', 'preset_rad_s3', 'imported'], itemPath);
          const source = stringAt(row.source, `${itemPath}.source`);
          if (!['off', 'override', 'imported', 'preset'].includes(source)) {
            return fail(itemPath, 'a known jerk-limit source');
          }
          const maximumRadS3 = nullableNumberAt(row.maximum_rad_s3, `${itemPath}.maximum_rad_s3`);
          const presetRadS3 = nullableNumberAt(row.preset_rad_s3, `${itemPath}.preset_rad_s3`);
          const imported = row.imported === null ? null : (() => {
            const importedPath = `${itemPath}.imported`;
            const info = objectAt(row.imported, importedPath);
            exactKeysAt(info, ['maximum_rad_s3', 'enabled', 'machine_required', 'source_id', 'source_digest'], importedPath);
            return {
              maximumRadS3: positiveNumberAt(info.maximum_rad_s3, `${importedPath}.maximum_rad_s3`),
              enabled: booleanAt(info.enabled, `${importedPath}.enabled`),
              machineRequired: booleanAt(info.machine_required, `${importedPath}.machine_required`),
              sourceId: stringAt(info.source_id, `${importedPath}.source_id`),
              sourceDigest: digestAt(info.source_digest, `${importedPath}.source_digest`),
            };
          })();
          if ((enabled ? maximumRadS3 === null || source === 'off' : maximumRadS3 !== null || source !== 'off')
            || (maximumRadS3 !== null && maximumRadS3 <= 0)
            || (presetRadS3 !== null && presetRadS3 <= 0)
            || (source === 'preset' && maximumRadS3 !== presetRadS3)
            || (source === 'imported' && (!imported?.enabled || maximumRadS3 !== imported.maximumRadS3))
            || (imported?.enabled && maximumRadS3 !== null && maximumRadS3 > imported.maximumRadS3)
            || (imported?.machineRequired && (!enabled || !imported.enabled))) {
            return fail(itemPath, 'consistent effective jerk limits and provenance');
          }
          return {
            sourceJointName: stringAt(row.source_joint_name, `${itemPath}.source_joint_name`),
            source: source as 'off' | 'override' | 'imported' | 'preset',
            maximumRadS3, presetRadS3, imported,
          };
        });
      if (jointLimits.length !== joints.length || jointLimits.some((row, index) => row.sourceJointName !== joints[index].sourceJointName)) {
        return fail(base, 'jerk limits joined to the exact ordered model joints');
      }
      return { enabled, presetReferenceTimeS: positiveNumberAt(profile.preset_reference_time_s, `${base}.preset_reference_time_s`), jointLimits };
    })();
  const operationalJointLimits = boundedArrayAt(
    record.operational_joint_limits,
    `${path}.operational_joint_limits`,
    1024,
  ).map((item, index) => {
    const itemPath = `${path}.operational_joint_limits[${index}]`;
    const limit = objectAt(item, itemPath);
    exactKeysAt(limit, [
      'deployment_max_acceleration_rad_s2', 'deployment_max_jerk_rad_s3',
      'deployment_max_velocity_rad_s', 'effective_max_acceleration_rad_s2',
      'effective_max_jerk_rad_s3', 'effective_max_velocity_rad_s',
      'model_max_acceleration_rad_s2', 'model_max_velocity_rad_s',
      'source_joint_name',
    ], itemPath);
    const result = {
      sourceJointName: stringAt(limit.source_joint_name, `${itemPath}.source_joint_name`),
      modelMaxVelocityRadS: positiveNumberAt(
        limit.model_max_velocity_rad_s,
        `${itemPath}.model_max_velocity_rad_s`,
      ),
      modelMaxAccelerationRadS2: nullableNumberAt(
        limit.model_max_acceleration_rad_s2,
        `${itemPath}.model_max_acceleration_rad_s2`,
      ),
      deploymentMaxVelocityRadS: nullableNumberAt(
        limit.deployment_max_velocity_rad_s,
        `${itemPath}.deployment_max_velocity_rad_s`,
      ),
      deploymentMaxAccelerationRadS2: nullableNumberAt(
        limit.deployment_max_acceleration_rad_s2,
        `${itemPath}.deployment_max_acceleration_rad_s2`,
      ),
      deploymentMaxJerkRadS3: nullableNumberAt(
        limit.deployment_max_jerk_rad_s3,
        `${itemPath}.deployment_max_jerk_rad_s3`,
      ),
      effectiveMaxVelocityRadS: positiveNumberAt(
        limit.effective_max_velocity_rad_s,
        `${itemPath}.effective_max_velocity_rad_s`,
      ),
      effectiveMaxAccelerationRadS2: nullableNumberAt(
        limit.effective_max_acceleration_rad_s2,
        `${itemPath}.effective_max_acceleration_rad_s2`,
      ),
      effectiveMaxJerkRadS3: nullableNumberAt(
        limit.effective_max_jerk_rad_s3,
        `${itemPath}.effective_max_jerk_rad_s3`,
      ),
    };
    for (const [name, value] of Object.entries(result)) {
      if (name !== 'sourceJointName' && value !== null && typeof value === 'number' && value <= 0) {
        return fail(itemPath, 'positive dynamics limits');
      }
    }
    const expectedVelocity = result.deploymentMaxVelocityRadS === null
      ? result.modelMaxVelocityRadS
      : Math.min(result.modelMaxVelocityRadS, result.deploymentMaxVelocityRadS);
    const expectedAcceleration = result.modelMaxAccelerationRadS2 === null
      ? result.deploymentMaxAccelerationRadS2
      : result.deploymentMaxAccelerationRadS2 === null
        ? result.modelMaxAccelerationRadS2
        : Math.min(
            result.modelMaxAccelerationRadS2,
            result.deploymentMaxAccelerationRadS2,
          );
    if (
      result.effectiveMaxVelocityRadS !== expectedVelocity
      || result.effectiveMaxAccelerationRadS2 !== expectedAcceleration
      || result.effectiveMaxJerkRadS3 !== (ordinaryJerk === null
        ? result.deploymentMaxJerkRadS3
        : !ordinaryJerk.enabled ? null
          : Math.min(result.deploymentMaxJerkRadS3 ?? Infinity,
            ordinaryJerk.jointLimits[index].maximumRadS3!))
    ) {
      return fail(itemPath, 'the exact conservative Deployment/model minimum');
    }
    return result;
  });
  if (
    operationalJointLimits.length !== joints.length
    || operationalJointLimits.some((limit, index) => (
      limit.sourceJointName !== joints[index]?.sourceJointName
      || limit.modelMaxVelocityRadS !== joints[index]?.maxVelocityRadS
      || limit.modelMaxAccelerationRadS2 !== joints[index]?.maxAccelerationRadS2
    ))
  ) {
    return fail(`${path}.operational_joint_limits`, 'limits joined conservatively to each source joint');
  }
  return {
    schemaVersion,
    controllableComponentId: stringAt(record.controllable_component_id, `${path}.controllable_component_id`),
    commandTargetId: stringAt(record.command_target_id, `${path}.command_target_id`),
    componentRoleId: stringAt(record.component_role_id, `${path}.component_role_id`),
    targetRoleId: stringAt(record.target_role_id, `${path}.target_role_id`),
    frames,
    joints,
    operationalJointLimits,
    ordinaryJerk,
    operationalLimit,
    contracts: manualControlContractsAt(record.contracts, `${path}.contracts`),
  };
}

function manualControlGripperDescriptorAt(
  value: unknown,
  path: string,
): ManualControlGripperDescriptor {
  const record = objectAt(value, path);
  exactKeysAt(record, [
    'actuation_kind', 'attach_frame', 'auxiliary_frames', 'closed_state_ref',
    'command_duration_s', 'command_kind', 'command_target_id',
    'component_role_id', 'completion_policy', 'contracts',
    'control_interface_contract_id', 'controllable_component_id',
    'coordinate_kind', 'description', 'grasp_confirmation_policy',
    'max_position', 'mimic_followers', 'min_position', 'mount_frame',
    'named_presets', 'open_state_ref', 'schema_version',
    'source_coordinate_name', 'source_group_name', 'source_kind',
    'state_authority_class', 'state_authority_id', 'target_role_id',
    'tcp_frame', 'touch_links', 'transport_kind', 'unit',
  ], path);
  const schemaVersion = stringAt(record.schema_version, `${path}.schema_version`);
  if (schemaVersion !== 'manyforge.manual_control_gripper_descriptor.v2') {
    return fail(`${path}.schema_version`, 'the supported gripper descriptor');
  }
  const coordinateKind = stringAt(record.coordinate_kind, `${path}.coordinate_kind`);
  if (coordinateKind !== 'angular_rad' && coordinateKind !== 'linear_m') {
    return fail(`${path}.coordinate_kind`, 'angular_rad or linear_m');
  }
  const unit = stringAt(record.unit, `${path}.unit`);
  if ((coordinateKind === 'angular_rad' && unit !== 'rad') || (coordinateKind === 'linear_m' && unit !== 'm')) {
    return fail(`${path}.unit`, 'the coordinate-kind unit');
  }
  const sourceKind = stringAt(record.source_kind, `${path}.source_kind`);
  if (sourceKind !== 'model_role' && sourceKind !== 'component_local') {
    return fail(`${path}.source_kind`, 'model_role or component_local');
  }
  const mimicFollowers = boundedArrayAt(record.mimic_followers, `${path}.mimic_followers`, 1024).map((item, index) => {
    const itemPath = `${path}.mimic_followers[${index}]`;
    const follower = objectAt(item, itemPath);
    exactKeysAt(follower, ['compiled_coordinate_name', 'multiplier', 'offset', 'source_coordinate_name'], itemPath);
    return {
      compiledCoordinateName: stringAt(follower.compiled_coordinate_name, `${itemPath}.compiled_coordinate_name`),
      sourceCoordinateName: stringAt(follower.source_coordinate_name, `${itemPath}.source_coordinate_name`),
      multiplier: numberAt(follower.multiplier, `${itemPath}.multiplier`),
      offset: numberAt(follower.offset, `${itemPath}.offset`),
    };
  });
  const namedPresets = boundedArrayAt(record.named_presets, `${path}.named_presets`, 1024).map((item, index) => {
    const itemPath = `${path}.named_presets[${index}]`;
    const preset = objectAt(item, itemPath);
    exactKeysAt(preset, ['coordinates', 'master_position', 'name'], itemPath);
    return {
      name: stringAt(preset.name, `${itemPath}.name`),
      masterPosition: numberAt(preset.master_position, `${itemPath}.master_position`),
      coordinates: boundedArrayAt(preset.coordinates, `${itemPath}.coordinates`, 1024).map((coordinate, coordinateIndex) => {
        const coordinatePath = `${itemPath}.coordinates[${coordinateIndex}]`;
        const row = objectAt(coordinate, coordinatePath);
        exactKeysAt(row, ['position', 'source_coordinate_name'], coordinatePath);
        return {
          sourceCoordinateName: stringAt(row.source_coordinate_name, `${coordinatePath}.source_coordinate_name`),
          position: numberAt(row.position, `${coordinatePath}.position`),
        };
      }),
    };
  });
  const minPosition = nullableNumberAt(record.min_position, `${path}.min_position`);
  const maxPosition = nullableNumberAt(record.max_position, `${path}.max_position`);
  const description = nullableStringAt(record.description, `${path}.description`);
  const actuationKind = nullableStringAt(record.actuation_kind, `${path}.actuation_kind`);
  const commandDurationS = nullableNumberAt(record.command_duration_s, `${path}.command_duration_s`);
  const openStateRef = nullableStringAt(record.open_state_ref, `${path}.open_state_ref`);
  const closedStateRef = nullableStringAt(record.closed_state_ref, `${path}.closed_state_ref`);
  const sourceGroupName = nullableStringAt(record.source_group_name, `${path}.source_group_name`);
  const mountFrame = nullableStringAt(record.mount_frame, `${path}.mount_frame`);
  const attachFrame = nullableStringAt(record.attach_frame, `${path}.attach_frame`);
  const tcpFrame = nullableStringAt(record.tcp_frame, `${path}.tcp_frame`);
  const controlInterfaceContractId = nullableStringAt(
    record.control_interface_contract_id,
    `${path}.control_interface_contract_id`,
  );
  const stateAuthorityId = nullableStringAt(record.state_authority_id, `${path}.state_authority_id`);
  const stateAuthorityClass = nullableStringAt(
    record.state_authority_class,
    `${path}.state_authority_class`,
  );
  const strings = (raw: unknown, field: string) => boundedArrayAt(
    raw,
    `${path}.${field}`,
    256,
  ).map((item, index) => {
    const result = stringAt(item, `${path}.${field}[${index}]`);
    if (result.trim() === '') return fail(`${path}.${field}[${index}]`, 'a non-empty source name');
    return result;
  });
  const touchLinks = strings(record.touch_links, 'touch_links');
  const auxiliaryFrames = strings(record.auxiliary_frames, 'auxiliary_frames');
  const contracts = manualControlContractsAt(record.contracts, `${path}.contracts`);
  const commandKind = stringAt(record.command_kind, `${path}.command_kind`);
  const transportKind = stringAt(record.transport_kind, `${path}.transport_kind`);
  const completionPolicy = stringAt(record.completion_policy, `${path}.completion_policy`);
  const graspConfirmationPolicy = stringAt(
    record.grasp_confirmation_policy,
    `${path}.grasp_confirmation_policy`,
  );
  if (
    commandKind !== 'gripper_position'
    || transportKind !== 'resolved_binding'
    || completionPolicy !== 'goal_fenced_target_reached'
    || graspConfirmationPolicy !== 'binding_feedback_required'
    || (actuationKind !== null
      && actuationKind !== 'position_commanded'
      && actuationKind !== 'pneumatic')
    || (stateAuthorityClass !== null
      && stateAuthorityClass !== 'measured'
      && stateAuthorityClass !== 'commanded'
      && stateAuthorityClass !== 'predicted')
  ) {
    return fail(path, 'the exact qualified gripper command semantics');
  }
  const authoredValues = [
    description, actuationKind, commandDurationS, openStateRef, closedStateRef,
    sourceGroupName, mountFrame, attachFrame, tcpFrame,
  ];
  const configured = authoredValues.some((item) => item !== null)
    || touchLinks.length > 0
    || auxiliaryFrames.length > 0;
  if (
    (configured && (
      authoredValues.some((item) => item === null)
      || description?.trim() === ''
      || commandDurationS === null
      || commandDurationS <= 0
      || commandDurationS > 60
      || touchLinks.length === 0
      || !namedPresets.some((preset) => preset.name === openStateRef)
      || !namedPresets.some((preset) => preset.name === closedStateRef)
    ))
    || new Set(touchLinks).size !== touchLinks.length
    || new Set(auxiliaryFrames).size !== auxiliaryFrames.length
    || (stateAuthorityId === null) !== (stateAuthorityClass === null)
    || controlInterfaceContractId !== (contracts.controlPath?.controlInterfaceContractId ?? null)
  ) {
    return fail(path, 'one complete, internally consistent gripper configuration');
  }
  if (
    (sourceKind === 'model_role'
      && (minPosition === null || maxPosition === null || minPosition > maxPosition))
    || (sourceKind === 'component_local'
      && (minPosition !== null || maxPosition !== null))
  ) {
    return fail(
      path,
      sourceKind === 'model_role'
        ? 'ordered model-backed limits'
        : 'no fabricated limits for a component-local coordinate',
    );
  }
  if (
    new Set(mimicFollowers.map(
      (follower) => follower.compiledCoordinateName,
    )).size !== mimicFollowers.length
    || new Set(namedPresets.map((preset) => preset.name)).size
      !== namedPresets.length
    || namedPresets.some((preset) => (
      new Set(preset.coordinates.map(
        (coordinate) => coordinate.sourceCoordinateName,
      )).size !== preset.coordinates.length
      || (minPosition !== null && preset.masterPosition < minPosition)
      || (maxPosition !== null && preset.masterPosition > maxPosition)
    ))
  ) {
    return fail(
      `${path}.named_presets`,
      'unique bounded presets, coordinates, and mimic followers',
    );
  }
  return {
    schemaVersion,
    controllableComponentId: stringAt(record.controllable_component_id, `${path}.controllable_component_id`),
    commandTargetId: stringAt(record.command_target_id, `${path}.command_target_id`),
    componentRoleId: stringAt(record.component_role_id, `${path}.component_role_id`),
    targetRoleId: stringAt(record.target_role_id, `${path}.target_role_id`),
    description,
    commandKind,
    actuationKind: actuationKind as ManualControlGripperDescriptor['actuationKind'],
    transportKind,
    controlInterfaceContractId,
    stateAuthorityId,
    stateAuthorityClass: stateAuthorityClass as ManualControlGripperDescriptor['stateAuthorityClass'],
    completionPolicy,
    graspConfirmationPolicy,
    sourceGroupName,
    sourceCoordinateName: stringAt(record.source_coordinate_name, `${path}.source_coordinate_name`),
    sourceKind: sourceKind as ResourceSourceKind,
    coordinateKind: coordinateKind as CoordinateKind,
    unit: unit as 'rad' | 'm',
    minPosition,
    maxPosition,
    openStateRef,
    closedStateRef,
    commandDurationS,
    touchLinks,
    mountFrame,
    attachFrame,
    tcpFrame,
    auxiliaryFrames,
    mimicFollowers,
    namedPresets,
    contracts,
  };
}

function executionIdentityAt(value: unknown, path: string): ExecutionIdentity {
  const identity = objectAt(value, path);
  exactKeysAt(identity, [
    'active_publication_generation',
    'cell_id',
    'control_management_mode',
    'graph_revision',
    'gripper_command_schema_id',
    'gripper_command_schema_revision',
    'gripper_target_role_id',
    'motion_target_role_id',
    'owner_epoch',
    'program_binding_id',
    'program_id',
    'program_revision',
    'retained_bundle_id',
    'scene_revision',
    'schema_version',
  ], path);
  if (stringAt(identity.schema_version, `${path}.schema_version`)
      !== 'manyforge.execution_identity.v1') {
    return fail(`${path}.schema_version`, 'manyforge.execution_identity.v1');
  }
  const controlManagementMode = stringAt(
    identity.control_management_mode,
    `${path}.control_management_mode`,
  );
  if (!['pending', 'direct', 'component_lifecycle'].includes(controlManagementMode)) {
    return fail(
      `${path}.control_management_mode`,
      'pending, direct, or component_lifecycle',
    );
  }
  return {
    ownerEpoch: stringAt(identity.owner_epoch, `${path}.owner_epoch`),
    cellId: stringAt(identity.cell_id, `${path}.cell_id`),
    activePublicationGeneration: positiveIntegerAt(
      identity.active_publication_generation,
      `${path}.active_publication_generation`,
    ),
    controlManagementMode: controlManagementMode as ExecutionIdentity['controlManagementMode'],
    graphRevision: stringAt(identity.graph_revision, `${path}.graph_revision`),
    retainedBundleId: stringAt(
      identity.retained_bundle_id,
      `${path}.retained_bundle_id`,
    ),
    sceneRevision: sceneRevisionAt(
      identity.scene_revision,
      `${path}.scene_revision`,
    ),
    programId: stringAt(identity.program_id, `${path}.program_id`),
    programRevision: stringAt(
      identity.program_revision,
      `${path}.program_revision`,
    ),
    programBindingId: stringAt(
      identity.program_binding_id,
      `${path}.program_binding_id`,
    ),
    motionTargetRoleId: stringAt(
      identity.motion_target_role_id,
      `${path}.motion_target_role_id`,
    ),
    gripperTargetRoleId: stringAt(
      identity.gripper_target_role_id,
      `${path}.gripper_target_role_id`,
    ),
    gripperCommandSchemaId: stringAt(
      identity.gripper_command_schema_id,
      `${path}.gripper_command_schema_id`,
    ),
    gripperCommandSchemaRevision: stringAt(
      identity.gripper_command_schema_revision,
      `${path}.gripper_command_schema_revision`,
    ),
  };
}

export function parseExecutionIdentity(value: unknown): ExecutionIdentity {
  const envelope = objectAt(value, 'executionIdentity');
  exactKeysAt(
    envelope,
    ['api_version', 'command', 'identity', 'ok'],
    'executionIdentity',
  );
  if (
    stringAt(envelope.api_version, 'executionIdentity.api_version')
      !== 'manyforge.execution_command_response.v1'
    || booleanAt(envelope.ok, 'executionIdentity.ok') !== true
    || stringAt(envelope.command, 'executionIdentity.command') !== 'identity'
  ) {
    return fail('executionIdentity', 'a successful identity response');
  }
  return executionIdentityAt(envelope.identity, 'executionIdentity.identity');
}

export function parseExecutionControlStatus(
  targetsValue: unknown,
  controlValue: unknown,
): ExecutionOwnerStatus {
  const targetsEnvelope = objectAt(targetsValue, 'executionTargets');
  exactKeysAt(
    targetsEnvelope,
    ['api_version', 'command', 'identity', 'ok', 'targets'],
    'executionTargets',
  );
  if (
    stringAt(targetsEnvelope.api_version, 'executionTargets.api_version')
      !== 'manyforge.execution_command_response.v1'
    || booleanAt(targetsEnvelope.ok, 'executionTargets.ok') !== true
    || stringAt(targetsEnvelope.command, 'executionTargets.command') !== 'targets'
  ) {
    return fail('executionTargets', 'a successful targets response');
  }
  const controlEnvelope = objectAt(controlValue, 'executionControlState');
  exactKeysAt(
    controlEnvelope,
    ['api_version', 'command', 'control_state', 'identity', 'ok'],
    'executionControlState',
  );
  if (
    stringAt(controlEnvelope.api_version, 'executionControlState.api_version')
      !== 'manyforge.execution_command_response.v1'
    || booleanAt(controlEnvelope.ok, 'executionControlState.ok') !== true
    || stringAt(controlEnvelope.command, 'executionControlState.command')
      !== 'control_state'
  ) {
    return fail('executionControlState', 'a successful control-state response');
  }
  const targetIdentity = executionIdentityAt(
    targetsEnvelope.identity,
    'executionTargets.identity',
  );
  const controlIdentity = executionIdentityAt(
    controlEnvelope.identity,
    'executionControlState.identity',
  );
  if (JSON.stringify(targetIdentity) !== JSON.stringify(controlIdentity)) {
    return fail('executionControlStatus', 'matching owner/product identities');
  }
  const targets = objectAt(targetsEnvelope.targets, 'executionTargets.targets');
  exactKeysAt(
    targets,
    ['direct_control', 'direct_control_gripper', 'schema_version'],
    'executionTargets.targets',
  );
  if (stringAt(targets.schema_version, 'executionTargets.targets.schema_version')
      !== 'manyforge.execution_targets.v1') {
    return fail(
      'executionTargets.targets.schema_version',
      'manyforge.execution_targets.v1',
    );
  }
  const control = objectAt(
    controlEnvelope.control_state,
    'executionControlState.control_state',
  );
  exactKeysAt(
    control,
    [
      'cell_stop_action',
      'observed_gripper',
      'observed_motion',
      'manual_operations',
      'runtime_scale_control',
      'schema_version',
    ],
    'executionControlState.control_state',
  );
  if (stringAt(
    control.schema_version,
    'executionControlState.control_state.schema_version',
  ) !== 'manyforge.execution_control_state.v4') {
    return fail(
      'executionControlState.control_state.schema_version',
      'manyforge.execution_control_state.v4',
    );
  }
  const motionTarget = objectAt(
    targets.direct_control,
    'executionTargets.targets.direct_control',
  );
  const gripperTarget = objectAt(
    targets.direct_control_gripper,
    'executionTargets.targets.direct_control_gripper',
  );
  const identity = targetIdentity;
  const allowedProgramRuns = new Set([
    stringAt(
      motionTarget.program_run_id,
      'executionTargets.targets.direct_control.program_run_id',
    ),
    stringAt(
      gripperTarget.program_run_id,
      'executionTargets.targets.direct_control_gripper.program_run_id',
    ),
  ]);
  const manualRows = arrayAt(
    control.manual_operations,
    'executionControlState.control_state.manual_operations',
  );
  if (manualRows.length > allowedProgramRuns.size) {
    return fail('executionControlState.control_state.manual_operations', 'at most one result per addressed manual target');
  }
  const manualOperations = manualRows.map((value, index) => {
    const projection = objectAt(
      value,
      `executionControlState.control_state.manual_operations[${index}]`,
    );
    exactKeysAt(projection, ['operation', 'resident'],
      `executionControlState.control_state.manual_operations[${index}]`);
    const operation = objectAt(projection.operation,
      `executionControlState.control_state.manual_operations[${index}].operation`);
    const programRunId = stringAt(
      operation.program_run_id,
      `executionControlState.control_state.manual_operations[${index}].program_run_id`,
    );
    if (!allowedProgramRuns.has(programRunId)) {
      return fail(
        `executionControlState.control_state.manual_operations[${index}].program_run_id`,
        'one of the exact direct-control Program runs',
      );
    }
    const observed = parseOperationObservation({
      command: 'read',
      ok: true,
      operation,
      resident: projection.resident,
    });
    if (!observed.resident && ![
      'completed', 'stopped', 'failed', 'interrupted', 'intervention_required', 'cancelled_pre_dispatch',
    ].includes(observed.state)) {
      return fail('executionControlState.control_state.manual_operations',
        'a terminal state for nonresident results');
    }
    return { ...observed, programRunId };
  });
  if (new Set(manualOperations.map((operation) => operation.operationId)).size
    !== manualOperations.length
    || new Set(manualOperations.map((operation) => operation.programRunId)).size
      !== manualOperations.length) {
    return fail(
      'executionControlState.control_state.manual_operations',
      'unique operation and manual-target identities',
    );
  }
  const status = parseExecutionOwnerStatus({
    api_version: 'manyforge.execution_command_response.v1',
    command: 'status',
    ok: true,
    status: {
      active_publication_generation: identity.activePublicationGeneration,
      cell_id: identity.cellId,
      cell_stop_action: control.cell_stop_action,
      direct_control: {
        ...motionTarget,
        observed_state: control.observed_motion,
      },
      direct_control_gripper: {
        ...gripperTarget,
        observed_state: control.observed_gripper,
      },
      graph_revision: identity.graphRevision,
      owner_epoch: identity.ownerEpoch,
      retained_bundle_id: identity.retainedBundleId,
      scene_revision: identity.sceneRevision,
    },
  });
  const runtimeScaleControl = runtimeScaleControlStatusAt(
    control.runtime_scale_control,
    'executionControlState.control_state.runtime_scale_control',
  );
  if (runtimeScaleControl.selector.kind === 'group') {
    return fail(
      'executionControlState.control_state.runtime_scale_control.selector',
      'the exact direct-control motion target, not a synchronized group',
    );
  }
  if (runtimeScaleControl.selector.kind === 'singular' && (
    runtimeScaleControl.selector.programBindingId !== status.directControl.programBindingId
    || runtimeScaleControl.selector.componentRoleId !== status.directControl.componentRoleId
    || runtimeScaleControl.selector.targetRoleId !== status.directControl.targetRoleId
  )) {
    return fail(
      'executionControlState.control_state.runtime_scale_control.selector',
      'the exact direct-control motion target',
    );
  }
  return {
    ...status,
    manualOperations,
    runtimeScaleControl,
  };
}

export function parseExecutionOwnerStatus(value: unknown): ExecutionOwnerStatus {
  const envelope = objectAt(value, 'executionStatus');
  if (
    booleanAt(envelope.ok, 'executionStatus.ok') !== true
    || stringAt(envelope.command, 'executionStatus.command') !== 'status'
  ) {
    return fail('executionStatus', 'a successful status response');
  }
  const status = objectAt(envelope.status, 'executionStatus.status');
  const direct = objectAt(
    status.direct_control,
    'executionStatus.status.direct_control',
  );
  exactKeysAt(direct, [
    'base_frame_role_id',
    'command_target_id',
    'component_role_id',
    'controllable_component_id',
    'frame_role_ids',
    'joints',
    'observed_state',
    'operation_capability_id',
    'planning_capability_ids',
    'program_binding_id',
    'program_id',
    'program_revision',
    'program_run_id',
    'target_role_id',
    'target_descriptor',
    'tcp_frame_role_id',
  ], 'executionStatus.status.direct_control');
  const motionDescriptor = manualControlMotionDescriptorAt(
    direct.target_descriptor,
    'executionStatus.status.direct_control.target_descriptor',
  );
  const joints = arrayAt(
    direct.joints,
    'executionStatus.status.direct_control.joints',
  ).map((item, index) => directControlJointAt(
    item,
    `executionStatus.status.direct_control.joints[${index}]`,
  ));
  if (joints.length === 0) {
    return fail('executionStatus.status.direct_control.joints', 'non-empty');
  }
  const sourceNames = joints.map((joint) => joint.sourceJointName);
  if (new Set(sourceNames).size !== sourceNames.length) {
    return fail(
      'executionStatus.status.direct_control.joints',
      'unique source joint names',
    );
  }
  for (const [index, joint] of joints.entries()) {
    if (
      joint.minPositionRad > joint.maxPositionRad
      || joint.maxVelocityRadS <= 0
      || (joint.maxAccelerationRadS2 !== null
        && joint.maxAccelerationRadS2 <= 0)
    ) {
      return fail(
        `executionStatus.status.direct_control.joints[${index}]`,
        'ordered finite limits',
      );
    }
  }
  const componentId = stringAt(
    direct.controllable_component_id,
    'executionStatus.status.direct_control.controllable_component_id',
  );
  const targetId = stringAt(
    direct.command_target_id,
    'executionStatus.status.direct_control.command_target_id',
  );
  const observed = objectAt(
    direct.observed_state,
    'executionStatus.status.direct_control.observed_state',
  );
  exactKeysAt(observed, [
    'complete',
    'current_tcp_pose',
    'positions_rad',
    'sequence',
    'source_epoch',
    'source_joint_names',
  ], 'executionStatus.status.direct_control.observed_state');
  const observedNames = stringArrayAt(
    observed.source_joint_names,
    'executionStatus.observedMotion.joint_names',
  );
  const positions = arrayAt(
    observed.positions_rad,
    'executionStatus.observedMotion.positions_rad',
  ).map((item, index) => numberAt(
    item,
    `executionStatus.observedMotion.positions_rad[${index}]`,
  ));
  if (
    observedNames.length !== sourceNames.length
    || observedNames.some((name, index) => name !== sourceNames[index])
    || positions.length !== sourceNames.length
  ) {
    return fail(
      'executionStatus.observedMotion',
      'the exact source-local direct-control joint order',
    );
  }
  const observedMotion: ObservedMotionTarget = {
    targetRoleId: stringAt(
      direct.target_role_id,
      'executionStatus.status.direct_control.target_role_id',
    ),
    controllableComponentId: componentId,
    commandTargetId: targetId,
    sequence: integerAt(
      observed.sequence,
      'executionStatus.observedMotion.sequence',
    ),
    sourceEpoch: stringAt(
      observed.source_epoch,
      'executionStatus.observedMotion.source_epoch',
    ),
    sourceJointNames: observedNames,
    positionsRad: positions,
    complete: booleanAt(
      observed.complete,
      'executionStatus.observedMotion.complete',
    ),
    currentTcpPose: poseAt(
      observed.current_tcp_pose,
      'executionStatus.observedMotion.current_tcp_pose',
    ),
  };
  const directGripper = objectAt(
    status.direct_control_gripper,
    'executionStatus.status.direct_control_gripper',
  );
  exactKeysAt(directGripper, [
    'command_schema_id',
    'command_schema_revision',
    'command_target_id',
    'component_role_id',
    'controllable_component_id',
    'observed_state',
    'operation_capability_id',
    'program_binding_id',
    'program_id',
    'program_revision',
    'program_run_id',
    'resource',
    'target_role_id',
    'target_descriptor',
  ], 'executionStatus.status.direct_control_gripper');
  const gripperDescriptor = manualControlGripperDescriptorAt(
    directGripper.target_descriptor,
    'executionStatus.status.direct_control_gripper.target_descriptor',
  );
  const gripperResource = objectAt(
    directGripper.resource,
    'executionStatus.status.direct_control_gripper.resource',
  );
  exactKeysAt(gripperResource, [
    'coordinate_kind',
    'max_position',
    'min_position',
    'source_coordinate_name',
    'source_kind',
  ], 'executionStatus.status.direct_control_gripper.resource');
  const coordinateKind = stringAt(
    gripperResource.coordinate_kind,
    'executionStatus.status.direct_control_gripper.resource.coordinate_kind',
  );
  if (coordinateKind !== 'angular_rad' && coordinateKind !== 'linear_m') {
    return fail(
      'executionStatus.status.direct_control_gripper.resource.coordinate_kind',
      'angular_rad or linear_m',
    );
  }
  const sourceKind = stringAt(
    gripperResource.source_kind,
    'executionStatus.status.direct_control_gripper.resource.source_kind',
  );
  if (sourceKind !== 'model_role' && sourceKind !== 'component_local') {
    return fail(
      'executionStatus.status.direct_control_gripper.resource.source_kind',
      'model_role or component_local',
    );
  }
  const gripperMin = nullableNumberAt(
    gripperResource.min_position,
    'executionStatus.status.direct_control_gripper.resource.min_position',
  );
  const gripperMax = nullableNumberAt(
    gripperResource.max_position,
    'executionStatus.status.direct_control_gripper.resource.max_position',
  );
  if (
    (sourceKind === 'model_role'
      && (gripperMin === null
        || gripperMax === null
        || gripperMin > gripperMax))
    || (sourceKind === 'component_local'
      && (gripperMin !== null || gripperMax !== null))
  ) {
    return fail(
      'executionStatus.status.direct_control_gripper.resource',
      sourceKind === 'model_role'
        ? 'ordered finite model limits'
        : 'no fabricated model limits for a component-local coordinate',
    );
  }
  const gripperSourceName = stringAt(
    gripperResource.source_coordinate_name,
    'executionStatus.status.direct_control_gripper.resource.source_coordinate_name',
  );
  if (directGripper.observed_state === null && sourceKind !== 'component_local') {
    return fail(
      'executionStatus.status.direct_control_gripper.observed_state',
      'a model-backed observation',
    );
  }
  let observedGripper: ObservedGripperTarget | null = null;
  if (directGripper.observed_state !== null) {
    const gripperObserved = objectAt(
      directGripper.observed_state,
      'executionStatus.status.direct_control_gripper.observed_state',
    );
    exactKeysAt(gripperObserved, [
      'complete',
      'position',
      'sequence',
      'source_coordinate_name',
      'source_epoch',
    ], 'executionStatus.status.direct_control_gripper.observed_state');
    const observedPosition = objectAt(
      gripperObserved.position,
      'executionStatus.status.direct_control_gripper.observed_state.position',
    );
    const expectedPositionKey = coordinateKind === 'angular_rad'
      ? 'value_rad'
      : 'value_m';
    exactKeysAt(
      observedPosition,
      ['kind', expectedPositionKey],
      'executionStatus.status.direct_control_gripper.observed_state.position',
    );
    if (stringAt(
      observedPosition.kind,
      'executionStatus.status.direct_control_gripper.observed_state.position.kind',
    ) !== coordinateKind) {
      return fail(
        'executionStatus.status.direct_control_gripper.observed_state.position.kind',
        coordinateKind,
      );
    }
    if (stringAt(
      gripperObserved.source_coordinate_name,
      'executionStatus.status.direct_control_gripper.observed_state.source_coordinate_name',
    ) !== gripperSourceName) {
      return fail(
        'executionStatus.status.direct_control_gripper.observed_state.source_coordinate_name',
        'the exact retained source coordinate',
      );
    }
    observedGripper = {
      sequence: integerAt(
        gripperObserved.sequence,
        'executionStatus.status.direct_control_gripper.observed_state.sequence',
      ),
      sourceEpoch: stringAt(
        gripperObserved.source_epoch,
        'executionStatus.status.direct_control_gripper.observed_state.source_epoch',
      ),
      sourceCoordinateName: gripperSourceName,
      position: numberAt(
        observedPosition[expectedPositionKey],
        `executionStatus.status.direct_control_gripper.observed_state.position.${expectedPositionKey}`,
      ),
      coordinateKind: coordinateKind as CoordinateKind,
      complete: booleanAt(
        gripperObserved.complete,
        'executionStatus.status.direct_control_gripper.observed_state.complete',
      ),
    };
  }
  const certificateRefAt = (input: unknown, path: string) => {
    const record = objectAt(input, path);
    exactKeysAt(record, [
      'stop_certificate_id', 'stop_certificate_revision',
      'stop_generation', 'stop_id',
    ], path);
    return {
      stopCertificateId: stringAt(
        record.stop_certificate_id,
        `${path}.stop_certificate_id`,
      ),
      stopCertificateRevision: positiveIntegerAt(
        record.stop_certificate_revision,
        `${path}.stop_certificate_revision`,
      ),
      stopId: stringAt(record.stop_id, `${path}.stop_id`),
      stopGeneration: positiveIntegerAt(
        record.stop_generation,
        `${path}.stop_generation`,
      ),
    };
  };
  const cellStop = objectAt(
    status.cell_stop_action,
    'executionStatus.status.cell_stop_action',
  );
  exactKeysAt(cellStop, [
    'available', 'expected_stop_generation', 'fault', 'latest_terminal',
    'unavailable_reason',
  ], 'executionStatus.status.cell_stop_action');
  const cellStopAvailable = booleanAt(
    cellStop.available,
    'executionStatus.status.cell_stop_action.available',
  );
  const cellStopUnavailableReason = nullableStringAt(
    cellStop.unavailable_reason,
    'executionStatus.status.cell_stop_action.unavailable_reason',
  );
  if (cellStopAvailable === (cellStopUnavailableReason !== null)) {
    return fail(
      'executionStatus.status.cell_stop_action',
      'an unavailable reason exactly when the action is unavailable',
    );
  }
  let latestTerminal: ExecutionOwnerStatus['cellStopAction']['latestTerminal'] = null;
  if (cellStop.latest_terminal !== null) {
    const terminal = objectAt(
      cellStop.latest_terminal,
      'executionStatus.status.cell_stop_action.latest_terminal',
    );
    exactKeysAt(terminal, [
      'certificate', 'escalated', 'executed_stages',
      'hard_stop_floor_used', 'result',
    ], 'executionStatus.status.cell_stop_action.latest_terminal');
    const result = stringAt(
      terminal.result,
      'executionStatus.status.cell_stop_action.latest_terminal.result',
    );
    if (![
      'succeeded_nominal', 'succeeded_escalated', 'partial',
      'failed_uncontained',
    ].includes(result)) {
      return fail(
        'executionStatus.status.cell_stop_action.latest_terminal.result',
        'a Stop certificate result',
      );
    }
    const executedStages = arrayAt(
      terminal.executed_stages,
      'executionStatus.status.cell_stop_action.latest_terminal.executed_stages',
    ).map((item, index) => {
      const path = `executionStatus.status.cell_stop_action.latest_terminal.executed_stages[${index}]`;
      const stage = objectAt(item, path);
      exactKeysAt(stage, [
        'action_class', 'executed_mode', 'operation_id', 'outcome',
        'participant_id', 'stage_id', 'stage_index', 'substitution_reason',
      ], path);
      const actionClass = stringAt(stage.action_class, `${path}.action_class`);
      if (!['controlled', 'software_hard', 'safety_system_request'].includes(actionClass)) {
        return fail(`${path}.action_class`, 'a Stop action class');
      }
      const outcome = stringAt(stage.outcome, `${path}.outcome`);
      if (!['succeeded', 'partial', 'failed', 'superseded_no_effect'].includes(outcome)) {
        return fail(`${path}.outcome`, 'a Stop stage outcome');
      }
      const executedMode = nullableStringAt(
        stage.executed_mode,
        `${path}.executed_mode`,
      );
      if (executedMode !== null && ![
        'soft_on_path', 'spring_back_hold', 'hard_deceleration',
      ].includes(executedMode)) {
        return fail(`${path}.executed_mode`, 'a controlled Stop mode');
      }
      return {
        operationId: stringAt(stage.operation_id, `${path}.operation_id`),
        participantId: stringAt(stage.participant_id, `${path}.participant_id`),
        stageId: stringAt(stage.stage_id, `${path}.stage_id`),
        stageIndex: integerAt(stage.stage_index, `${path}.stage_index`),
        actionClass: actionClass as 'controlled' | 'software_hard' | 'safety_system_request',
        outcome: outcome as 'succeeded' | 'partial' | 'failed' | 'superseded_no_effect',
        executedMode: executedMode as 'soft_on_path' | 'spring_back_hold' | 'hard_deceleration' | null,
        substitutionReason: nullableStringAt(
          stage.substitution_reason,
          `${path}.substitution_reason`,
        ),
      };
    });
    latestTerminal = {
      certificate: certificateRefAt(
        terminal.certificate,
        'executionStatus.status.cell_stop_action.latest_terminal.certificate',
      ),
      result: result as 'succeeded_nominal' | 'succeeded_escalated' | 'partial' | 'failed_uncontained',
      escalated: booleanAt(
        terminal.escalated,
        'executionStatus.status.cell_stop_action.latest_terminal.escalated',
      ),
      hardStopFloorUsed: booleanAt(
        terminal.hard_stop_floor_used,
        'executionStatus.status.cell_stop_action.latest_terminal.hard_stop_floor_used',
      ),
      executedStages,
    };
  }
  let cellStopFault: ExecutionOwnerStatus['cellStopAction']['fault'] = null;
  if (cellStop.fault !== null) {
    const path = 'executionStatus.status.cell_stop_action.fault';
    const fault = objectAt(cellStop.fault, path);
    exactKeysAt(fault, [
      'certificate', 'latch_generation', 'latch_id', 'reason_code',
      'recovery_status', 'state', 'trigger',
    ], path);
    const stateName = stringAt(fault.state, `${path}.state`);
    if (!['latched', 'recovery_pending', 'cleared'].includes(stateName)) {
      return fail(`${path}.state`, 'a Stop fault latch state');
    }
    const trigger = stringAt(fault.trigger, `${path}.trigger`);
    if (![
      'scope_promotion', 'escalated', 'partial', 'failed_uncontained',
      'physical_rest_unproven', 'persistence_degraded',
    ].includes(trigger)) {
      return fail(`${path}.trigger`, 'a Stop fault trigger');
    }
    const recoveryStatus = stringAt(
      fault.recovery_status,
      `${path}.recovery_status`,
    );
    if (![
      'waiting_for_observation', 'waiting_for_quiescence',
      'waiting_for_configuration', 'waiting_for_external_reconciliation',
      'waiting_for_durable_record', 'ready', 'recovery_pending', 'cleared',
    ].includes(recoveryStatus)) {
      return fail(`${path}.recovery_status`, 'a Stop fault recovery status');
    }
    cellStopFault = {
      latchId: stringAt(fault.latch_id, `${path}.latch_id`),
      latchGeneration: positiveIntegerAt(
        fault.latch_generation,
        `${path}.latch_generation`,
      ),
      state: stateName as 'latched' | 'recovery_pending' | 'cleared',
      trigger: trigger as NonNullable<ExecutionOwnerStatus['cellStopAction']['fault']>['trigger'],
      recoveryStatus: recoveryStatus as NonNullable<ExecutionOwnerStatus['cellStopAction']['fault']>['recoveryStatus'],
      reasonCode: stringAt(fault.reason_code, `${path}.reason_code`),
      certificate: fault.certificate === null
        ? null
        : certificateRefAt(fault.certificate, `${path}.certificate`),
    };
  }
  const frameRoleIds = stringArrayAt(
    direct.frame_role_ids,
    'executionStatus.status.direct_control.frame_role_ids',
  );
  const planningCapabilityIds = stringArrayAt(
    direct.planning_capability_ids,
    'executionStatus.status.direct_control.planning_capability_ids',
  );
  const descriptorFrameRoleIds = motionDescriptor.frames.map(
    (frame) => frame.frameRoleId,
  );
  const descriptorSupportingCapabilityIds = motionDescriptor.contracts
    .supportingProviders.map((provider) => provider.capabilityId);
  if (
    motionDescriptor.controllableComponentId !== componentId
    || motionDescriptor.commandTargetId !== targetId
    || motionDescriptor.componentRoleId !== direct.component_role_id
    || motionDescriptor.targetRoleId !== direct.target_role_id
    || gripperDescriptor.controllableComponentId !== directGripper.controllable_component_id
    || gripperDescriptor.commandTargetId !== directGripper.command_target_id
    || gripperDescriptor.componentRoleId !== directGripper.component_role_id
    || gripperDescriptor.targetRoleId !== directGripper.target_role_id
    || gripperDescriptor.sourceCoordinateName !== gripperSourceName
    || gripperDescriptor.sourceKind !== sourceKind
    || gripperDescriptor.coordinateKind !== coordinateKind
    || gripperDescriptor.minPosition !== gripperMin
    || gripperDescriptor.maxPosition !== gripperMax
    || motionDescriptor.contracts.operationCapabilityId !== direct.operation_capability_id
    || gripperDescriptor.contracts.operationCapabilityId !== directGripper.operation_capability_id
    || gripperDescriptor.contracts.commandSchema?.commandSchemaId !== directGripper.command_schema_id
    || gripperDescriptor.contracts.commandSchema?.commandSchemaRevision !== directGripper.command_schema_revision
    || frameRoleIds.length !== descriptorFrameRoleIds.length
    || frameRoleIds.some((role, index) => role !== descriptorFrameRoleIds[index])
    || planningCapabilityIds.length !== descriptorSupportingCapabilityIds.length
    || planningCapabilityIds.some(
      (capability, index) => capability !== descriptorSupportingCapabilityIds[index],
    )
    || motionDescriptor.joints.length !== joints.length
    || motionDescriptor.joints.some((joint, index) => {
      const statusJoint = joints[index];
      return statusJoint === undefined
        || joint.sourceJointName !== statusJoint.sourceJointName
        || joint.minPositionRad !== statusJoint.minPositionRad
        || joint.maxPositionRad !== statusJoint.maxPositionRad
        || joint.maxVelocityRadS !== statusJoint.maxVelocityRadS
        || joint.maxAccelerationRadS2 !== statusJoint.maxAccelerationRadS2;
    })
  ) {
    return fail('executionStatus.status.direct_control', 'descriptors joined to the exact addressed targets');
  }
  return {
    ownerEpoch: stringAt(status.owner_epoch, 'executionStatus.status.owner_epoch'),
    cellId: stringAt(status.cell_id, 'executionStatus.status.cell_id'),
    activePublicationGeneration: positiveIntegerAt(
      status.active_publication_generation,
      'executionStatus.status.active_publication_generation',
    ),
    graphRevision: stringAt(
      status.graph_revision,
      'executionStatus.status.graph_revision',
    ),
    retainedBundleId: stringAt(
      status.retained_bundle_id,
      'executionStatus.status.retained_bundle_id',
    ),
    sceneRevision: sceneRevisionAt(
      status.scene_revision,
      'executionStatus.status.scene_revision',
    ),
    directControl: {
      controllableComponentId: componentId,
      commandTargetId: targetId,
      componentRoleId: stringAt(
        direct.component_role_id,
        'executionStatus.status.direct_control.component_role_id',
      ),
      targetRoleId: stringAt(
        direct.target_role_id,
        'executionStatus.status.direct_control.target_role_id',
      ),
      baseFrameRoleId: stringAt(
        direct.base_frame_role_id,
        'executionStatus.status.direct_control.base_frame_role_id',
      ),
      tcpFrameRoleId: stringAt(
        direct.tcp_frame_role_id,
        'executionStatus.status.direct_control.tcp_frame_role_id',
      ),
      operationCapabilityId: stringAt(
        direct.operation_capability_id,
        'executionStatus.status.direct_control.operation_capability_id',
      ),
      planningCapabilityIds,
      frameRoleIds,
      programId: stringAt(
        direct.program_id,
        'executionStatus.status.direct_control.program_id',
      ),
      programRevision: stringAt(
        direct.program_revision,
        'executionStatus.status.direct_control.program_revision',
      ),
      programBindingId: stringAt(
        direct.program_binding_id,
        'executionStatus.status.direct_control.program_binding_id',
      ),
      programRunId: stringAt(
        direct.program_run_id,
        'executionStatus.status.direct_control.program_run_id',
      ),
      joints,
      targetDescriptor: motionDescriptor,
    },
    directControlGripper: {
      controllableComponentId: stringAt(
        directGripper.controllable_component_id,
        'executionStatus.status.direct_control_gripper.controllable_component_id',
      ),
      commandTargetId: stringAt(
        directGripper.command_target_id,
        'executionStatus.status.direct_control_gripper.command_target_id',
      ),
      componentRoleId: stringAt(
        directGripper.component_role_id,
        'executionStatus.status.direct_control_gripper.component_role_id',
      ),
      targetRoleId: stringAt(
        directGripper.target_role_id,
        'executionStatus.status.direct_control_gripper.target_role_id',
      ),
      operationCapabilityId: stringAt(
        directGripper.operation_capability_id,
        'executionStatus.status.direct_control_gripper.operation_capability_id',
      ),
      programId: stringAt(
        directGripper.program_id,
        'executionStatus.status.direct_control_gripper.program_id',
      ),
      programRevision: stringAt(
        directGripper.program_revision,
        'executionStatus.status.direct_control_gripper.program_revision',
      ),
      programBindingId: stringAt(
        directGripper.program_binding_id,
        'executionStatus.status.direct_control_gripper.program_binding_id',
      ),
      programRunId: stringAt(
        directGripper.program_run_id,
        'executionStatus.status.direct_control_gripper.program_run_id',
      ),
      commandSchemaId: stringAt(
        directGripper.command_schema_id,
        'executionStatus.status.direct_control_gripper.command_schema_id',
      ),
      commandSchemaRevision: stringAt(
        directGripper.command_schema_revision,
        'executionStatus.status.direct_control_gripper.command_schema_revision',
      ),
      sourceCoordinateName: gripperSourceName,
      sourceKind: sourceKind as ResourceSourceKind,
      coordinateKind: coordinateKind as CoordinateKind,
      minPosition: gripperMin,
      maxPosition: gripperMax,
      targetDescriptor: gripperDescriptor,
    },
    observedMotion,
    observedGripper,
    manualOperations: [],
    runtimeScaleControl: null,
    cellStopAction: {
      available: cellStopAvailable,
      unavailableReason: cellStopUnavailableReason,
      expectedStopGeneration: integerAt(
        cellStop.expected_stop_generation,
        'executionStatus.status.cell_stop_action.expected_stop_generation',
      ),
      latestTerminal,
      fault: cellStopFault,
    },
  };
}

function poseAt(value: unknown, path: string): MotionPreviewReceipt['finalTcpPose'] {
  const pose = objectAt(value, path);
  const position = objectAt(pose.position_m, `${path}.position_m`);
  const orientation = objectAt(pose.orientation, `${path}.orientation`);
  exactKeysAt(pose, ['orientation', 'position_m'], path);
  exactKeysAt(position, ['x_m', 'y_m', 'z_m'], `${path}.position_m`);
  exactKeysAt(orientation, ['w', 'x', 'y', 'z'], `${path}.orientation`);
  return {
    position_m: {
      x_m: numberAt(position.x_m, `${path}.position_m.x_m`),
      y_m: numberAt(position.y_m, `${path}.position_m.y_m`),
      z_m: numberAt(position.z_m, `${path}.position_m.z_m`),
    },
    orientation: {
      x: numberAt(orientation.x, `${path}.orientation.x`),
      y: numberAt(orientation.y, `${path}.orientation.y`),
      z: numberAt(orientation.z, `${path}.orientation.z`),
      w: numberAt(orientation.w, `${path}.orientation.w`),
    },
  };
}

export function parseManualControlAvailability(value: unknown): ManualControlAvailability {
  const path = 'manualControlAvailability';
  const envelope = objectAt(value, path);
  exactKeysAt(envelope, ['api_version', 'command', 'ok', 'request_id',
    'request_owner_epoch', 'intent_digest', 'availability', 'manual_handover'], path);
  if (envelope.api_version !== 'manyforge.execution_command_response.v1'
    || envelope.command !== 'read_manual_handover' || envelope.ok !== true) {
    return fail(path, 'a successful manual availability response');
  }
  const flags = objectAt(envelope.availability, `${path}.availability`);
  exactKeysAt(flags, ['available', 'unavailable_reason', 'cleanup_required'], `${path}.availability`);
  const available = booleanAt(flags.available, `${path}.available`);
  const unavailableReason = nullableStringAt(flags.unavailable_reason, `${path}.unavailable_reason`);
  const cleanupRequired = booleanAt(flags.cleanup_required, `${path}.cleanup_required`);
  let manualHandover: ManualProgramHandoverContext | null = null;
  if (envelope.manual_handover !== null) {
    const context = objectAt(envelope.manual_handover, `${path}.manual_handover`);
    exactKeysAt(context, ['program_runner_owner_epoch', 'program_run_id',
      'claim_generation', 'selection_revision'], `${path}.manual_handover`);
    const selection = integerAt(context.selection_revision, `${path}.selection_revision`);
    manualHandover = {
      program_runner_owner_epoch: stringAt(context.program_runner_owner_epoch, `${path}.program_runner_owner_epoch`),
      program_run_id: stringAt(context.program_run_id, `${path}.program_run_id`),
      claim_generation: positiveIntegerAt(context.claim_generation, `${path}.claim_generation`),
      selection_revision: selection,
    };
  }
  if (available === (unavailableReason !== null)
    || (cleanupRequired && (!available || manualHandover === null))) {
    return fail(path, 'consistent availability, reason and cleanup context');
  }
  return {
    requestId: stringAt(envelope.request_id, `${path}.request_id`),
    requestOwnerEpoch: stringAt(envelope.request_owner_epoch, `${path}.request_owner_epoch`),
    intentDigest: digestAt(envelope.intent_digest, `${path}.intent_digest`),
    available, unavailableReason, cleanupRequired, manualHandover,
  };
}

export function parseMotionPreviewResponse(value: unknown): MotionPreviewReceipt {
  const envelope = objectAt(value, 'motionPreview');
  if (
    booleanAt(envelope.ok, 'motionPreview.ok') !== true
    || stringAt(envelope.command, 'motionPreview.command') !== 'preview_motion'
  ) {
    return fail('motionPreview', 'a successful preview response');
  }
  const receipt = objectAt(
    envelope.preview_receipt,
    'motionPreview.preview_receipt',
  );
  exactKeysAt(receipt, [
    'collision_world_revision',
    'commit_eligible',
    'component_role_id',
    'final_positions_rad',
    'final_tcp_pose',
    'graph_revision',
    'intent_digest',
    'program_binding_id',
    'program_id',
    'program_revision',
    'program_run_id',
    'receipt_digest',
    'request_id',
    'request_owner_epoch',
    'required_capability_id',
    'retained_bundle_id',
    'scene_revision',
    'schema_version',
    'source_joint_names',
    'start_positions_rad',
    'target_role_id',
  ], 'motionPreview.preview_receipt');
  const schemaVersion = stringAt(
    receipt.schema_version,
    'motionPreview.preview_receipt.schema_version',
  );
  if (schemaVersion !== 'manyforge.direct_control_motion_preview.v1') {
    return fail(
      'motionPreview.preview_receipt.schema_version',
      'manyforge.direct_control_motion_preview.v1',
    );
  }
  const sourceJointNames = stringArrayAt(
    receipt.source_joint_names,
    'motionPreview.preview_receipt.source_joint_names',
  );
  const startPositionsRad = arrayAt(
    receipt.start_positions_rad,
    'motionPreview.preview_receipt.start_positions_rad',
  ).map((item, index) => numberAt(
    item,
    `motionPreview.preview_receipt.start_positions_rad[${index}]`,
  ));
  const finalPositionsRad = arrayAt(
    receipt.final_positions_rad,
    'motionPreview.preview_receipt.final_positions_rad',
  ).map((item, index) => numberAt(
    item,
    `motionPreview.preview_receipt.final_positions_rad[${index}]`,
  ));
  if (
    sourceJointNames.length === 0
    || new Set(sourceJointNames).size !== sourceJointNames.length
    || startPositionsRad.length !== sourceJointNames.length
    || finalPositionsRad.length !== sourceJointNames.length
  ) {
    return fail(
      'motionPreview.preview_receipt',
      'equal non-empty unique joint arrays',
    );
  }
  const collision = objectAt(
    receipt.collision_world_revision,
    'motionPreview.preview_receipt.collision_world_revision',
  );
  exactKeysAt(
    collision,
    ['content_hash', 'revision_id'],
    'motionPreview.preview_receipt.collision_world_revision',
  );
  exactKeysAt(
    objectAt(receipt.scene_revision, 'motionPreview.preview_receipt.scene_revision'),
    ['content_hash', 'revision_id', 'scene_id'],
    'motionPreview.preview_receipt.scene_revision',
  );
  const collisionRevisionId = integerAt(
    collision.revision_id,
    'motionPreview.preview_receipt.collision_world_revision.revision_id',
  );
  if (collisionRevisionId < 1) {
    return fail(
      'motionPreview.preview_receipt.collision_world_revision.revision_id',
      'positive',
    );
  }
  if (booleanAt(
    receipt.commit_eligible,
    'motionPreview.preview_receipt.commit_eligible',
  ) !== true) {
    return fail(
      'motionPreview.preview_receipt.commit_eligible',
      'true',
    );
  }
  return {
    schemaVersion,
    receiptDigest: stringAt(
      receipt.receipt_digest,
      'motionPreview.preview_receipt.receipt_digest',
    ),
    intentDigest: stringAt(
      receipt.intent_digest,
      'motionPreview.preview_receipt.intent_digest',
    ),
    requestId: stringAt(
      receipt.request_id,
      'motionPreview.preview_receipt.request_id',
    ),
    requestOwnerEpoch: stringAt(
      receipt.request_owner_epoch,
      'motionPreview.preview_receipt.request_owner_epoch',
    ),
    programId: stringAt(
      receipt.program_id,
      'motionPreview.preview_receipt.program_id',
    ),
    programRevision: stringAt(
      receipt.program_revision,
      'motionPreview.preview_receipt.program_revision',
    ),
    programBindingId: stringAt(
      receipt.program_binding_id,
      'motionPreview.preview_receipt.program_binding_id',
    ),
    programRunId: stringAt(
      receipt.program_run_id,
      'motionPreview.preview_receipt.program_run_id',
    ),
    graphRevision: stringAt(
      receipt.graph_revision,
      'motionPreview.preview_receipt.graph_revision',
    ),
    retainedBundleId: stringAt(
      receipt.retained_bundle_id,
      'motionPreview.preview_receipt.retained_bundle_id',
    ),
    sceneRevision: sceneRevisionAt(
      receipt.scene_revision,
      'motionPreview.preview_receipt.scene_revision',
    ),
    componentRoleId: stringAt(
      receipt.component_role_id,
      'motionPreview.preview_receipt.component_role_id',
    ),
    targetRoleId: stringAt(
      receipt.target_role_id,
      'motionPreview.preview_receipt.target_role_id',
    ),
    requiredCapabilityId: stringAt(
      receipt.required_capability_id,
      'motionPreview.preview_receipt.required_capability_id',
    ),
    sourceJointNames,
    startPositionsRad,
    finalPositionsRad,
    finalTcpPose: poseAt(
      receipt.final_tcp_pose,
      'motionPreview.preview_receipt.final_tcp_pose',
    ),
    collisionWorldRevision: {
      revision_id: collisionRevisionId,
      content_hash: stringAt(
        collision.content_hash,
        'motionPreview.preview_receipt.collision_world_revision.content_hash',
      ),
    },
    commitEligible: true,
  };
}

export function parseCapabilityCommandPreviewResponse(
  value: unknown,
): CapabilityCommandPreviewReceipt {
  const envelope = objectAt(value, 'capabilityCommandPreview');
  if (
    booleanAt(envelope.ok, 'capabilityCommandPreview.ok') !== true
    || stringAt(
      envelope.command,
      'capabilityCommandPreview.command',
    ) !== 'preview_capability_command'
  ) {
    return fail(
      'capabilityCommandPreview',
      'a successful capability-command preview response',
    );
  }
  const receipt = objectAt(
    envelope.preview_receipt,
    'capabilityCommandPreview.preview_receipt',
  );
  exactKeysAt(receipt, [
    'command_schema_id',
    'command_schema_revision',
    'commit_eligible',
    'component_role_id',
    'coordinate_kind',
    'graph_revision',
    'intent_digest',
    'max_position',
    'min_position',
    'program_binding_id',
    'program_id',
    'program_revision',
    'program_run_id',
    'receipt_digest',
    'request_id',
    'request_owner_epoch',
    'required_capability_id',
    'retained_bundle_id',
    'scene_revision',
    'schema_version',
    'source_coordinate_name',
    'source_kind',
    'target_position',
    'target_role_id',
  ], 'capabilityCommandPreview.preview_receipt');
  const schemaVersion = stringAt(
    receipt.schema_version,
    'capabilityCommandPreview.preview_receipt.schema_version',
  );
  if (
    schemaVersion
    !== 'manyforge.direct_control_capability_command_preview.v1'
  ) {
    return fail(
      'capabilityCommandPreview.preview_receipt.schema_version',
      'manyforge.direct_control_capability_command_preview.v1',
    );
  }
  const coordinateKind = stringAt(
    receipt.coordinate_kind,
    'capabilityCommandPreview.preview_receipt.coordinate_kind',
  );
  if (coordinateKind !== 'angular_rad' && coordinateKind !== 'linear_m') {
    return fail(
      'capabilityCommandPreview.preview_receipt.coordinate_kind',
      'angular_rad or linear_m',
    );
  }
  const sourceKind = stringAt(
    receipt.source_kind,
    'capabilityCommandPreview.preview_receipt.source_kind',
  );
  if (sourceKind !== 'model_role' && sourceKind !== 'component_local') {
    return fail(
      'capabilityCommandPreview.preview_receipt.source_kind',
      'model_role or component_local',
    );
  }
  const minPosition = nullableNumberAt(
    receipt.min_position,
    'capabilityCommandPreview.preview_receipt.min_position',
  );
  const maxPosition = nullableNumberAt(
    receipt.max_position,
    'capabilityCommandPreview.preview_receipt.max_position',
  );
  const targetPosition = numberAt(
    receipt.target_position,
    'capabilityCommandPreview.preview_receipt.target_position',
  );
  if (
    (sourceKind === 'model_role'
      && (minPosition === null
        || maxPosition === null
        || minPosition > targetPosition
        || targetPosition > maxPosition))
    || (sourceKind === 'component_local'
      && (minPosition !== null || maxPosition !== null))
    || booleanAt(
      receipt.commit_eligible,
      'capabilityCommandPreview.preview_receipt.commit_eligible',
    ) !== true
  ) {
    return fail(
      'capabilityCommandPreview.preview_receipt',
      'an in-limit commit-eligible command',
    );
  }
  exactKeysAt(
    objectAt(
      receipt.scene_revision,
      'capabilityCommandPreview.preview_receipt.scene_revision',
    ),
    ['content_hash', 'revision_id', 'scene_id'],
    'capabilityCommandPreview.preview_receipt.scene_revision',
  );
  return {
    schemaVersion,
    receiptDigest: stringAt(
      receipt.receipt_digest,
      'capabilityCommandPreview.preview_receipt.receipt_digest',
    ),
    intentDigest: stringAt(
      receipt.intent_digest,
      'capabilityCommandPreview.preview_receipt.intent_digest',
    ),
    requestId: stringAt(
      receipt.request_id,
      'capabilityCommandPreview.preview_receipt.request_id',
    ),
    requestOwnerEpoch: stringAt(
      receipt.request_owner_epoch,
      'capabilityCommandPreview.preview_receipt.request_owner_epoch',
    ),
    programId: stringAt(
      receipt.program_id,
      'capabilityCommandPreview.preview_receipt.program_id',
    ),
    programRevision: stringAt(
      receipt.program_revision,
      'capabilityCommandPreview.preview_receipt.program_revision',
    ),
    programBindingId: stringAt(
      receipt.program_binding_id,
      'capabilityCommandPreview.preview_receipt.program_binding_id',
    ),
    programRunId: stringAt(
      receipt.program_run_id,
      'capabilityCommandPreview.preview_receipt.program_run_id',
    ),
    graphRevision: stringAt(
      receipt.graph_revision,
      'capabilityCommandPreview.preview_receipt.graph_revision',
    ),
    retainedBundleId: stringAt(
      receipt.retained_bundle_id,
      'capabilityCommandPreview.preview_receipt.retained_bundle_id',
    ),
    sceneRevision: sceneRevisionAt(
      receipt.scene_revision,
      'capabilityCommandPreview.preview_receipt.scene_revision',
    ),
    componentRoleId: stringAt(
      receipt.component_role_id,
      'capabilityCommandPreview.preview_receipt.component_role_id',
    ),
    targetRoleId: stringAt(
      receipt.target_role_id,
      'capabilityCommandPreview.preview_receipt.target_role_id',
    ),
    requiredCapabilityId: stringAt(
      receipt.required_capability_id,
      'capabilityCommandPreview.preview_receipt.required_capability_id',
    ),
    commandSchemaId: stringAt(
      receipt.command_schema_id,
      'capabilityCommandPreview.preview_receipt.command_schema_id',
    ),
    commandSchemaRevision: stringAt(
      receipt.command_schema_revision,
      'capabilityCommandPreview.preview_receipt.command_schema_revision',
    ),
    sourceCoordinateName: stringAt(
      receipt.source_coordinate_name,
      'capabilityCommandPreview.preview_receipt.source_coordinate_name',
    ),
    sourceKind: sourceKind as ResourceSourceKind,
    coordinateKind: coordinateKind as CoordinateKind,
    targetPosition,
    minPosition,
    maxPosition,
    commitEligible: true,
  };
}

const OPERATION_STATES = new Set<OperationStateName>([
  'queued',
  'resolving',
  'planning',
  'validating',
  'prepared',
  'dispatching',
  'executing',
  'paused',
  'stopping',
  'completed',
  'stopped',
  'failed',
  'interrupted',
  'intervention_required',
  'cancelled_pre_dispatch',
]);

export function parseOperationObservation(value: unknown): OperationObservation {
  const envelope = objectAt(value, 'operationObservation');
  if (
    booleanAt(envelope.ok, 'operationObservation.ok') !== true
    || stringAt(envelope.command, 'operationObservation.command') !== 'read'
  ) {
    return fail('operationObservation', 'a successful read response');
  }
  const operation = objectAt(
    envelope.operation,
    'operationObservation.operation',
  );
  const stateValue = stringAt(
    operation.state,
    'operationObservation.operation.state',
  ) as OperationStateName;
  if (!OPERATION_STATES.has(stateValue)) {
    return fail('operationObservation.operation.state', 'a known state');
  }
  const participants = arrayAt(
    operation.participants,
    'operationObservation.operation.participants',
  );
  if (participants.length !== 1) {
    return fail(
      'operationObservation.operation.participants',
      'one participant',
    );
  }
  const participant = objectAt(
    participants[0],
    'operationObservation.operation.participants[0]',
  );
  const progress = numberAt(
    participant.progress,
    'operationObservation.operation.participants[0].progress',
  );
  if (progress < 0 || progress > 1) {
    return fail(
      'operationObservation.operation.participants[0].progress',
      'within [0, 1]',
    );
  }
  let terminalError: string | null = null;
  if (participant.terminal_error !== null) {
    const error = objectAt(
      participant.terminal_error,
      'operationObservation.operation.participants[0].terminal_error',
    );
    terminalError = stringAt(
      error.message,
      'operationObservation.operation.participants[0].terminal_error.message',
    );
  }
  const revision = integerAt(
    operation.operation_revision,
    'operationObservation.operation.operation_revision',
  );
  if (revision < 1) {
    return fail('operationObservation.operation.operation_revision', 'positive');
  }
  return {
    operationId: stringAt(
      operation.operation_id,
      'operationObservation.operation.operation_id',
    ),
    operationRevision: revision,
    resident: booleanAt(
      envelope.resident,
      'operationObservation.resident',
    ),
    state: stateValue,
    progress,
    activeStopId: nullableStringAt(
      operation.active_stop_id,
      'operationObservation.operation.active_stop_id',
    ),
    activeStopGeneration: operation.active_stop_generation === null
      ? null
      : integerAt(
        operation.active_stop_generation,
        'operationObservation.operation.active_stop_generation',
      ),
    terminalError,
  };
}

const RUNTIME_SCALE_STATES = new Set([
  'pending',
  'applied',
  'rejected',
  'failed',
]);

function runtimeScalePortableAt(value: unknown, field: string): string {
  const result = stringAt(value, field);
  if (result.length === 0 || result !== result.normalize('NFC')
    || new TextEncoder().encode(result).length > 256) {
    return fail(field, 'a non-empty NFC-normalized identifier within 256 UTF-8 bytes');
  }
  return result;
}

function runtimeScaleControlSelectorAt(
  value: unknown,
  path: string,
): RuntimeScaleControlStatus['selector'] {
  const selector = objectAt(value, path);
  const binding = runtimeScalePortableAt(selector.program_binding_id, `${path}.program_binding_id`);
  if (Object.prototype.hasOwnProperty.call(selector, 'synchronized_targets')) {
    exactKeysAt(selector, ['program_binding_id', 'synchronized_targets'], path);
    const rows = arrayAt(selector.synchronized_targets, `${path}.synchronized_targets`);
    if (rows.length < 2 || rows.length > 4096) {
      return fail(`${path}.synchronized_targets`, 'between 2 and 4096 targets');
    }
    const targets = rows.map((row, index) => {
      const member = objectAt(row, `${path}.synchronized_targets[${index}]`);
      exactKeysAt(member, ['component_role_id', 'target_role_id'], `${path}.synchronized_targets[${index}]`);
      return {
        componentRoleId: runtimeScalePortableAt(member.component_role_id, `${path}.synchronized_targets[${index}].component_role_id`),
        targetRoleId: runtimeScalePortableAt(member.target_role_id, `${path}.synchronized_targets[${index}].target_role_id`),
      };
    });
    const identities = targets.map((target) => runtimeScalePairIdentity(
      target.componentRoleId,
      target.targetRoleId,
    ));
    if (new Set(identities).size !== identities.length) {
      return fail(`${path}.synchronized_targets`, 'unique target pairs');
    }
    return { kind: 'group', programBindingId: binding, synchronizedTargets: targets };
  }
  exactKeysAt(selector, ['component_role_id', 'program_binding_id', 'target_role_id'], path);
  return {
    kind: 'singular',
    componentRoleId: runtimeScalePortableAt(selector.component_role_id, `${path}.component_role_id`),
    programBindingId: binding,
    targetRoleId: runtimeScalePortableAt(selector.target_role_id, `${path}.target_role_id`),
  };
}

function nullableNonnegativeIntegerAt(
  value: unknown,
  path: string,
): number | null {
  if (value === null) return null;
  const result = integerAt(value, path);
  return result >= 0 ? result : fail(path, 'a nonnegative integer or null');
}

function nonnegativeIntegerAt(value: unknown, path: string): number {
  const result = nullableNonnegativeIntegerAt(value, path);
  return result === null ? fail(path, 'a nonnegative integer') : result;
}

function runtimeScaleControlStatusAt(
  value: unknown,
  path: string,
): RuntimeScaleControlStatus {
  const status = objectAt(value, path);
  const selector = runtimeScaleControlSelectorAt(status.selector, `${path}.selector`);
  exactKeysAt(status, [
    'active_control_configuration_generation',
    'active_control_configuration_id',
    'active_operation_id',
    'active_operation_revision',
    'active_operation_scale_generation',
    'application_state',
    'control_binding_id',
    'control_binding_revision',
    'control_domain_id',
    'control_domain_revision',
    'control_generation',
    'effective_scale',
    'frozen',
    'reason',
    'requested_scale',
    'scaling_domain_id',
    'selector',
    'supported',
    ...(selector.kind === 'group' ? ['synchronized_members'] : []),
  ], path);
  const controlGeneration = integerAt(
    status.control_generation,
    `${path}.control_generation`,
  );
  const requestedScale = numberAt(status.requested_scale, `${path}.requested_scale`);
  const effectiveScale = nullableNumberAt(status.effective_scale, `${path}.effective_scale`);
  if (
    controlGeneration < 0
    || requestedScale < 0 || requestedScale > 1
    || (effectiveScale !== null && (effectiveScale < 0 || effectiveScale > 1))
  ) {
    return fail(path, 'nonnegative generations and scales within [0, 1]');
  }
  const applicationState = nullableStringAt(
    status.application_state,
    `${path}.application_state`,
  );
  if (applicationState !== null && !RUNTIME_SCALE_STATES.has(applicationState)) {
    return fail(`${path}.application_state`, 'a known runtime-scale state or null');
  }
  let reason: string | null = null;
  if (status.reason !== null) {
    const error = objectAt(status.reason, `${path}.reason`);
    exactKeysAt(error, ['code', 'details', 'message'], `${path}.reason`);
    objectAt(error.details, `${path}.reason.details`);
    stringAt(error.code, `${path}.reason.code`);
    reason = stringAt(error.message, `${path}.reason.message`);
  }
  const controlDomainId = nullableStringAt(
    status.control_domain_id,
    `${path}.control_domain_id`,
  );
  const controlDomainRevision = nullableStringAt(
    status.control_domain_revision,
    `${path}.control_domain_revision`,
  );
  const controlBindingId = nullableStringAt(
    status.control_binding_id,
    `${path}.control_binding_id`,
  );
  const controlBindingRevision = nullableStringAt(
    status.control_binding_revision,
    `${path}.control_binding_revision`,
  );
  const scalingDomainId = nullableStringAt(
    status.scaling_domain_id,
    `${path}.scaling_domain_id`,
  );
  const activeControlConfigurationId = nullableStringAt(
    status.active_control_configuration_id,
    `${path}.active_control_configuration_id`,
  );
  const activeControlConfigurationGeneration = nullableNonnegativeIntegerAt(
    status.active_control_configuration_generation,
    `${path}.active_control_configuration_generation`,
  );
  const resolvedScope = [
    controlDomainId,
    controlDomainRevision,
    controlBindingId,
    controlBindingRevision,
    scalingDomainId,
    activeControlConfigurationId,
    activeControlConfigurationGeneration,
  ];
  const supported = booleanAt(status.supported, `${path}.supported`);
  if (selector.kind === 'singular' && supported !== resolvedScope.every((item) => item !== null)) {
    return fail(path, 'support truth consistent with one complete resolved control scope');
  }
  let synchronizedMembers: RuntimeScaleControlStatus['synchronizedMembers'];
  if (selector.kind === 'group') {
    const rows = arrayAt(status.synchronized_members, `${path}.synchronized_members`);
    if (rows.length < 2 || rows.length > 4096) {
      return fail(`${path}.synchronized_members`, 'between 2 and 4096 members');
    }
    const expected = new Set(selector.synchronizedTargets.map((target) => (
      runtimeScalePairIdentity(target.componentRoleId, target.targetRoleId)
    )));
    const seen = new Set<string>();
    if (!supported) return fail(path, 'a synchronized group must be supported');
    synchronizedMembers = rows.map((row, index) => {
      const member = objectAt(row, `${path}.synchronized_members[${index}]`);
      exactKeysAt(member, [
        'active_control_configuration_generation', 'active_control_configuration_id',
        'control_binding_id', 'control_binding_revision', 'control_domain_id',
        'control_domain_revision', 'scaling_domain_id', 'component_role_id', 'target_role_id',
      ], `${path}.synchronized_members[${index}]`);
      const componentRoleId = runtimeScalePortableAt(member.component_role_id, `${path}.synchronized_members[${index}].component_role_id`);
      const targetRoleId = runtimeScalePortableAt(member.target_role_id, `${path}.synchronized_members[${index}].target_role_id`);
      const identity = runtimeScalePairIdentity(componentRoleId, targetRoleId);
      if (!expected.has(identity) || seen.has(identity)) return fail(`${path}.synchronized_members[${index}]`, 'exactly one selected target');
      seen.add(identity);
      const fields = [
        'control_domain_id', 'control_domain_revision', 'control_binding_id',
        'control_binding_revision', 'scaling_domain_id', 'active_control_configuration_id',
      ].map((field) => runtimeScalePortableAt(member[field], `${path}.synchronized_members[${index}].${field}`));
      return {
        componentRoleId, targetRoleId,
        controlDomainId: fields[0], controlDomainRevision: fields[1], controlBindingId: fields[2],
        controlBindingRevision: fields[3], scalingDomainId: fields[4], activeControlConfigurationId: fields[5],
        activeControlConfigurationGeneration: nonnegativeIntegerAt(
          member.active_control_configuration_generation,
          `${path}.synchronized_members[${index}].active_control_configuration_generation`,
        ),
      };
    });
    if (seen.size !== expected.size || resolvedScope.some((item) => item !== null)) {
      return fail(path, 'group members must exactly resolve the selected scope while group scope stays null');
    }
  }
  return {
    selector,
    controlGeneration,
    requestedScale,
    supported,
    frozen: booleanAt(status.frozen, `${path}.frozen`),
    controlDomainId,
    controlDomainRevision,
    controlBindingId,
    controlBindingRevision,
    scalingDomainId,
    activeControlConfigurationId,
    activeControlConfigurationGeneration,
    activeOperationId: nullableStringAt(
      status.active_operation_id,
      `${path}.active_operation_id`,
    ),
    activeOperationRevision: nullableNonnegativeIntegerAt(
      status.active_operation_revision,
      `${path}.active_operation_revision`,
    ),
    activeOperationScaleGeneration: nullableNonnegativeIntegerAt(
      status.active_operation_scale_generation,
      `${path}.active_operation_scale_generation`,
    ),
    effectiveScale,
    applicationState: applicationState as RuntimeScaleControlStatus['applicationState'],
    reason,
    ...(synchronizedMembers === undefined ? {} : { synchronizedMembers }),
  };
}

export function parseRuntimeScaleControlStatus(
  value: unknown,
): RuntimeScaleControlStatus {
  const envelope = objectAt(value, 'runtimeScaleControl');
  exactKeysAt(
    envelope,
    ['api_version', 'command', 'ok', 'runtime_scale_control'],
    'runtimeScaleControl',
  );
  if (
    booleanAt(envelope.ok, 'runtimeScaleControl.ok') !== true
    || stringAt(envelope.command, 'runtimeScaleControl.command')
      !== 'get_runtime_scale_control'
  ) {
    return fail('runtimeScaleControl', 'a successful control-scope response');
  }
  return runtimeScaleControlStatusAt(
    envelope.runtime_scale_control,
    'runtimeScaleControl.runtime_scale_control',
  );
}

export function parseRuntimeScaleControlAccepted(
  value: unknown,
): RuntimeScaleControlAccepted {
  const envelope = objectAt(value, 'runtimeScaleControlAcceptance');
  exactKeysAt(
    envelope,
    ['accepted', 'api_version', 'command', 'ok'],
    'runtimeScaleControlAcceptance',
  );
  if (
    booleanAt(envelope.ok, 'runtimeScaleControlAcceptance.ok') !== true
    || stringAt(envelope.command, 'runtimeScaleControlAcceptance.command')
      !== 'set_runtime_scale_control'
  ) {
    return fail(
      'runtimeScaleControlAcceptance',
      'a successful control-scope acceptance',
    );
  }
  const accepted = objectAt(
    envelope.accepted,
    'runtimeScaleControlAcceptance.accepted',
  );
  exactKeysAt(accepted, [
    'accepted_at_ns',
    'active_operation_id',
    'active_operation_revision',
    'active_operation_scale_generation',
    'control_generation',
    'replay_status',
    'request_id',
    'requested_scale',
    'selector',
  ], 'runtimeScaleControlAcceptance.accepted');
  const controlGeneration = integerAt(
    accepted.control_generation,
    'runtimeScaleControlAcceptance.accepted.control_generation',
  );
  const requestedScale = numberAt(
    accepted.requested_scale,
    'runtimeScaleControlAcceptance.accepted.requested_scale',
  );
  if (controlGeneration < 1 || requestedScale < 0 || requestedScale > 1) {
    return fail(
      'runtimeScaleControlAcceptance.accepted',
      'a positive generation and scale within [0, 1]',
    );
  }
  stringAt(
    accepted.replay_status,
    'runtimeScaleControlAcceptance.accepted.replay_status',
  );
  monotonicNanosecondsAt(
    accepted.accepted_at_ns,
    'runtimeScaleControlAcceptance.accepted.accepted_at_ns',
  );
  return {
    requestId: stringAt(
      accepted.request_id,
      'runtimeScaleControlAcceptance.accepted.request_id',
    ),
    selector: runtimeScaleControlSelectorAt(
      accepted.selector,
      'runtimeScaleControlAcceptance.accepted.selector',
    ),
    requestedScale,
    controlGeneration,
    activeOperationId: nullableStringAt(
      accepted.active_operation_id,
      'runtimeScaleControlAcceptance.accepted.active_operation_id',
    ),
    activeOperationRevision: nullableNonnegativeIntegerAt(
      accepted.active_operation_revision,
      'runtimeScaleControlAcceptance.accepted.active_operation_revision',
    ),
    activeOperationScaleGeneration: nullableNonnegativeIntegerAt(
      accepted.active_operation_scale_generation,
      'runtimeScaleControlAcceptance.accepted.active_operation_scale_generation',
    ),
  };
}
