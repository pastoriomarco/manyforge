import type {
  AuthoredNeutralProjection,
  CatalogBlackboardBinding,
  CatalogCapabilityRequirement,
  CatalogEntry,
  CatalogParameter,
  CatalogResponse,
  FileBrowserEntry,
  FileBrowserResponse,
  HealthResponse,
  QuaternionXyzwTuple,
  SceneUrdfCollision,
  SceneUrdfJoint,
  SceneUrdfLink,
  SceneUrdfManifest,
  SceneUrdfOrigin,
  SceneUrdfPrimitive,
  SceneUrdfVisual,
  Vector3Tuple,
} from '../types/api';
import type {
  AlarmLifecycleState,
  AlarmMembership,
  AlarmPriority,
  AlarmSubject,
  ComposerHeadlessState,
  HeadlessRuntimeSnapshot,
  HeadlessSourceSection,
  HistorianEvent,
  HistorianPage,
  OperatorAlarmRecord,
  OperatorAlarmSnapshot,
  SourceFreshness,
} from '../types/headless';
import type {
  CapabilityCommandPreviewReceipt,
  CoordinateKind,
  DirectControlJoint,
  ExecutionOwnerStatus,
  MotionPreviewReceipt,
  ObservedGripperTarget,
  ObservedMotionTarget,
  OperationAcceptance,
  OperationObservation,
  OperationStateName,
  SceneRevisionValue,
} from '../types/robotControl';

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

function parseProjection(value: unknown): AuthoredNeutralProjection {
  const record = objectAt(value, 'manifest.projection');
  return {
    kind: literalAt(record.kind, 'authored_neutral', 'manifest.projection.kind'),
    source: literalAt(record.source, 'authored_urdf', 'manifest.projection.source'),
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
      'zero_independent_with_authored_mimic',
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
    robot_id: nullableStringAt(record.robot_id, 'manifest.robot_id'),
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
    activatedAtNs: integerAt(record.activated_at_ns, `${path}.activated_at_ns`),
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

export function parseComposerHeadlessState(
  value: unknown,
): ComposerHeadlessState {
  const record = objectAt(value, 'headlessState');
  const schemaVersion = stringAt(
    record.schema_version,
    'headlessState.schema_version',
  );
  if (schemaVersion !== 'manyforge.composer_headless_state.v1') {
    return fail(
      'headlessState.schema_version',
      'manyforge.composer_headless_state.v1',
    );
  }
  return {
    schemaVersion,
    runtime: parseHeadlessRuntimeSnapshot(record.runtime),
    alarms: parseOperatorAlarmSnapshot(record.alarms),
  };
}

export function parseObjectResponse(value: unknown): Record<string, unknown> {
  return objectAt(value, 'response');
}

function parseHistorianEvent(value: unknown, path: string): HistorianEvent {
  const record = objectAt(value, path);
  const schemaVersion = stringAt(record.schema_version, `${path}.schema_version`);
  if (schemaVersion !== 'manyforge.historian_event.v1') {
    return fail(`${path}.schema_version`, 'manyforge.historian_event.v1');
  }
  return {
    schemaVersion,
    eventId: stringAt(record.event_id, `${path}.event_id`),
    component: stringAt(record.component, `${path}.component`),
    eventType: stringAt(record.event_type, `${path}.event_type`),
    severity: stringAt(record.severity, `${path}.severity`),
    occurredAtNs: integerAt(record.occurred_at_ns, `${path}.occurred_at_ns`),
    occurredAtWall: stringAt(
      record.occurred_at_wall,
      `${path}.occurred_at_wall`,
    ),
    payload: objectAt(record.payload, `${path}.payload`),
  };
}

export function parseHistorianPage(value: unknown): HistorianPage {
  const record = objectAt(value, 'history');
  const schemaVersion = stringAt(record.schema_version, 'history.schema_version');
  if (schemaVersion !== 'manyforge.historian_page.v1') {
    return fail('history.schema_version', 'manyforge.historian_page.v1');
  }
  return {
    schemaVersion,
    events: arrayAt(record.events, 'history.events').map((value, index) => {
      const stored = objectAt(value, `history.events[${index}]`);
      return {
        sequence: integerAt(
          stored.sequence,
          `history.events[${index}].sequence`,
        ),
        event: parseHistorianEvent(
          stored.event,
          `history.events[${index}].event`,
        ),
      };
    }),
    nextBeforeSequence: record.next_before_sequence === null
      ? null
      : integerAt(record.next_before_sequence, 'history.next_before_sequence'),
    truncated: booleanAt(record.truncated, 'history.truncated'),
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
    'tcp_frame_role_id',
  ], 'executionStatus.status.direct_control');
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
  ], 'executionStatus.status.direct_control_gripper');
  const gripperResource = objectAt(
    directGripper.resource,
    'executionStatus.status.direct_control_gripper.resource',
  );
  exactKeysAt(gripperResource, [
    'coordinate_kind',
    'max_position',
    'min_position',
    'source_coordinate_name',
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
  const gripperMin = numberAt(
    gripperResource.min_position,
    'executionStatus.status.direct_control_gripper.resource.min_position',
  );
  const gripperMax = numberAt(
    gripperResource.max_position,
    'executionStatus.status.direct_control_gripper.resource.max_position',
  );
  if (gripperMin > gripperMax) {
    return fail(
      'executionStatus.status.direct_control_gripper.resource',
      'ordered finite position limits',
    );
  }
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
  const gripperSourceName = stringAt(
    gripperResource.source_coordinate_name,
    'executionStatus.status.direct_control_gripper.resource.source_coordinate_name',
  );
  if (stringAt(
    gripperObserved.source_coordinate_name,
    'executionStatus.status.direct_control_gripper.observed_state.source_coordinate_name',
  ) !== gripperSourceName) {
    return fail(
      'executionStatus.status.direct_control_gripper.observed_state.source_coordinate_name',
      'the exact retained source coordinate',
    );
  }
  const observedGripper: ObservedGripperTarget = {
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
  return {
    ownerEpoch: stringAt(status.owner_epoch, 'executionStatus.status.owner_epoch'),
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
      planningCapabilityIds: stringArrayAt(
        direct.planning_capability_ids,
        'executionStatus.status.direct_control.planning_capability_ids',
      ),
      frameRoleIds: stringArrayAt(
        direct.frame_role_ids,
        'executionStatus.status.direct_control.frame_role_ids',
      ),
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
      coordinateKind: coordinateKind as CoordinateKind,
      minPosition: gripperMin,
      maxPosition: gripperMax,
    },
    observedMotion,
    observedGripper,
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
  const minPosition = numberAt(
    receipt.min_position,
    'capabilityCommandPreview.preview_receipt.min_position',
  );
  const maxPosition = numberAt(
    receipt.max_position,
    'capabilityCommandPreview.preview_receipt.max_position',
  );
  const targetPosition = numberAt(
    receipt.target_position,
    'capabilityCommandPreview.preview_receipt.target_position',
  );
  if (
    minPosition > targetPosition
    || targetPosition > maxPosition
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
    coordinateKind: coordinateKind as CoordinateKind,
    targetPosition,
    minPosition,
    maxPosition,
    commitEligible: true,
  };
}

export function parseOperationAcceptance(value: unknown): OperationAcceptance {
  const envelope = objectAt(value, 'operationAcceptance');
  if (
    booleanAt(envelope.ok, 'operationAcceptance.ok') !== true
    || stringAt(envelope.command, 'operationAcceptance.command') !== 'submit'
  ) {
    return fail('operationAcceptance', 'a successful submit response');
  }
  const accepted = objectAt(envelope.accepted, 'operationAcceptance.accepted');
  const revision = integerAt(
    accepted.operation_revision,
    'operationAcceptance.accepted.operation_revision',
  );
  if (revision < 1) {
    return fail('operationAcceptance.accepted.operation_revision', 'positive');
  }
  return {
    requestId: stringAt(
      accepted.request_id,
      'operationAcceptance.accepted.request_id',
    ),
    operationId: stringAt(
      accepted.operation_id,
      'operationAcceptance.accepted.operation_id',
    ),
    operationRevision: revision,
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
