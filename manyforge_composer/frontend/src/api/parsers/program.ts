import type {
  CatalogBlackboardBinding,
  CatalogCapabilityRequirement,
  CatalogEntry,
  CatalogParameter,
  CatalogResponse,
  HealthResponse,
  StartupScenarioNotice,
  WorkbenchSessionNotice,
  JsonValue,
  ProgramDocument,
  ProgramAuthoringDocument,
  ProgramNodeRecoveryPolicy,
  ProgramParameterDeclaration,
  ProgramOperatorStopPolicy,
  ProgramSceneFrameRef,
  ProgramScenePose,
  ProgramSceneResource,
  ProgramSceneShape,
  ProgramTreeNode,
  ProgramValueSource,
  ProgramViewerMetadata,
} from '../../types/api';
import { PROGRAM_AUTHORED_VALUE_TYPES } from '../../types/api';
import { poseFrameOf } from '../../utils/programFrames';
import { overlappingSourcePaths, parseValuePath } from '../../utils/valuePaths';
import {
  allowedKeysAt,
  arrayAt,
  booleanAt,
  boundedArrayAt,
  canonicalStringArrayAt,
  digestAt,
  exactKeysAt,
  fail,
  integerAt,
  jsonRecordAt,
  jsonValueAt,
  nonEmptyStringAt,
  nullableStringAt,
  numberAt,
  objectAt,
  positiveNumberAt,
  quaternionAt,
  stringArrayAt,
  stringAt,
  stringRecordAt,
  vector3At,
} from './shared';

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
    effectful: booleanAt(record.effectful, `${path}.effectful`),
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

function parseWorkbenchSessionNotice(value: unknown): WorkbenchSessionNotice {
  const path = 'health.workbench_session_notice';
  const record = objectAt(value, path);
  exactKeysAt(record, [
    'schema_version', 'session_id', 'archive_relative_path', 'shutdown',
    'effects', 'history_coverage', 'journal_families', 'message',
  ], path);
  if (record.schema_version !== 'manyforge.workbench_previous_session.v1') {
    return fail(`${path}.schema_version`, 'manyforge.workbench_previous_session.v1');
  }
  const sessionId = stringAt(record.session_id, `${path}.session_id`);
  if (!/^[0-9a-f]{32}$/.test(sessionId)) {
    return fail(`${path}.session_id`, '32 lowercase hexadecimal characters');
  }
  const archiveRelativePath = stringAt(record.archive_relative_path, `${path}.archive_relative_path`);
  if (archiveRelativePath !== `previous-sessions/${sessionId}`) {
    return fail(`${path}.archive_relative_path`, 'the matching previous-session archive path');
  }
  if (record.shutdown !== 'clean' && record.shutdown !== 'unknown') {
    return fail(`${path}.shutdown`, 'clean or unknown');
  }
  if (record.effects !== 'closed' && record.effects !== 'unresolved' && record.effects !== 'unknown') {
    return fail(`${path}.effects`, 'closed, unresolved or unknown');
  }
  if (record.history_coverage !== 'not_imported') {
    return fail(`${path}.history_coverage`, 'not_imported');
  }
  const journalFamilies = stringArrayAt(record.journal_families, `${path}.journal_families`);
  const expectedFamilies = ['program_cycle', 'scene', 'manager', 'kernel_operation'];
  if (journalFamilies.length !== expectedFamilies.length
    || journalFamilies.some((family, index) => family !== expectedFamilies[index])) {
    return fail(`${path}.journal_families`, 'the four ordered journal families');
  }
  const message = stringAt(record.message, `${path}.message`);
  const messageBytes = new TextEncoder().encode(message);
  if (messageBytes.length > 1024 || new TextDecoder().decode(messageBytes) !== message
    || message.normalize('NFC') !== message) {
    return fail(`${path}.message`, 'NFC UTF-8 text of at most 1024 bytes');
  }
  return {
    schemaVersion: record.schema_version,
    sessionId,
    archiveRelativePath,
    shutdown: record.shutdown,
    effects: record.effects,
    historyCoverage: record.history_coverage,
    journalFamilies,
    message,
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
    workbenchSessionNotice: record.workbench_session_notice == null
      ? null
      : parseWorkbenchSessionNotice(record.workbench_session_notice),
    startupScenarioNotice: record.startup_scenario_notice == null
      ? null
      : parseStartupScenarioNotice(record.startup_scenario_notice),
  };
}

function parseStartupScenarioNotice(value: unknown): StartupScenarioNotice {
  const path = 'health.startup_scenario_notice';
  const record = objectAt(value, path);
  exactKeysAt(record, ['code', 'message', 'path'], path);
  return {
    path: stringAt(record.path, `${path}.path`),
    code: stringAt(record.code, `${path}.code`),
    message: stringAt(record.message, `${path}.message`),
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
  const kind = stringAt(record.kind, `${path}.kind`);
  if (kind === 'expression') {
    exactKeysAt(record, ['expression', 'kind'], path);
    return { kind, expression: nonEmptyStringAt(record.expression, `${path}.expression`) };
  }
  if (kind === 'blackboard_field') {
    exactKeysAt(record, ['field', 'kind', 'reference'], path);
    return {
      kind,
      reference: stringAt(record.reference, `${path}.reference`),
      field: stringAt(record.field, `${path}.field`),
    };
  }
  if (kind !== 'program_parameter' && kind !== 'blackboard_key') {
    return fail(`${path}.kind`, 'a supported Program value-source kind');
  }
  exactKeysAt(record, ['kind', 'reference'], path);
  return { kind, reference: stringAt(record.reference, `${path}.reference`) };
}

function parseProgramValueSources(value: unknown, path: string): Record<string, ProgramValueSource> {
  const sources = Object.fromEntries(Object.entries(objectAt(value, path)).map(([key, source]) => {
    if (parseValuePath(key) === null) return fail(`${path}.${key}`, 'keyed by a canonical value path');
    return [key, parseProgramValueSource(source, `${path}.${key}`)];
  }));
  const overlap = overlappingSourcePaths(sources)[0];
  if (overlap !== undefined) {
    return fail(`${path}.${overlap[1]}`, `absent while ${overlap[0]} is sourced as a whole`);
  }
  return sources;
}

const OPERATOR_STOP_POLICIES = new Set<ProgramOperatorStopPolicy>([
  'inherit',
  'hold',
  'continue',
]);

function parseProgramNodeRecoveryPolicy(
  value: unknown,
  path: string,
): ProgramNodeRecoveryPolicy {
  const record = objectAt(value, path);
  allowedKeysAt(record, ['allow_intervention', 'max_retries'], ['intervention_stop_scope'], path);
  const maxRetries = integerAt(record.max_retries, `${path}.max_retries`);
  if (maxRetries < 0 || maxRetries > 255) {
    return fail(`${path}.max_retries`, 'an integer in 0..255 (0 is Unlimited)');
  }
  const policy: ProgramNodeRecoveryPolicy = {
    allow_intervention: booleanAt(
      record.allow_intervention,
      `${path}.allow_intervention`,
    ),
    max_retries: maxRetries,
  };
  if (record.intervention_stop_scope !== undefined) {
    if (record.intervention_stop_scope === 'all') {
      return fail(`${path}.intervention_stop_scope`, 'omitted (all is the canonical default)');
    }
    if (record.intervention_stop_scope !== 'own_groups') {
      return fail(`${path}.intervention_stop_scope`, 'all or own_groups');
    }
    policy.intervention_stop_scope = 'own_groups';
  }
  return policy;
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
      'recovery_policy',
      'operator_stop_policy',
      'execution_groups',
    ],
    path,
  );
  const sources = record.value_sources === undefined
    ? undefined
    : parseProgramValueSources(record.value_sources, `${path}.value_sources`);
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
  if (
    node.recovery_policy !== undefined
    && !node.recovery_policy.allow_intervention
    && node.recovery_policy.max_retries === 0
  ) {
    return fail(`${path}.recovery_policy`, 'omit the canonical default policy');
  }
  if (node.operator_stop_policy === 'inherit') {
    return fail(`${path}.operator_stop_policy`, 'omit the canonical default policy');
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
    ['parameter_id', 'type', 'default'],
    ['constraints', 'description'],
    path,
  );
  const parameter: ProgramParameterDeclaration = {
    parameter_id: stringAt(record.parameter_id, `${path}.parameter_id`),
    type: stringAt(record.type, `${path}.type`),
    default: jsonValueAt(record.default, `${path}.default`),
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
    ['resource_roles', 'metadata'],
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
      'hidden_execution_groups',
    ],
    path,
  );
  const viewer: ProgramViewerMetadata = {};
  if (record.hidden_execution_groups !== undefined) {
    viewer.hidden_execution_groups = canonicalStringArrayAt(
      record.hidden_execution_groups,
      `${path}.hidden_execution_groups`,
    );
  }
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
      'execution_groups',
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
          ['fields', 'description', 'initial', 'expression', 'allowed_frames'],
          rowPath,
        );
        const type = stringAt(row.type, `${rowPath}.type`);
        const access = stringAt(row.access, `${rowPath}.access`);
        const persistence = stringAt(row.persistence, `${rowPath}.persistence`);
        if (!['read', 'write', 'read_write'].includes(access)) {
          return fail(`${rowPath}.access`, 'a supported access mode');
        }
        if (persistence !== 'run' && persistence !== 'persistent') {
          return fail(`${rowPath}.persistence`, 'run or persistent');
        }
        const initial = row.initial === undefined
          ? undefined
          : jsonValueAt(row.initial, `${rowPath}.initial`);
        if (initial === null) {
          return fail(`${rowPath}.initial`, 'omitted instead of null for an empty key');
        }
        if (initial !== undefined && !PROGRAM_AUTHORED_VALUE_TYPES.has(type)) {
          return fail(`${rowPath}.initial`, `omitted: ${type} has no authored value`);
        }
        const expression = row.expression === undefined
          ? undefined
          : nonEmptyStringAt(row.expression, `${rowPath}.expression`);
        if (expression !== undefined && (initial !== undefined || persistence === 'persistent')) {
          return fail(`${rowPath}.expression`, 'absent for a key with initial: or persistent');
        }
        const allowedFrames = row.allowed_frames === undefined
          ? undefined
          : arrayAt(row.allowed_frames, `${rowPath}.allowed_frames`).map((frame, frameIndex) => {
            const parsed = poseFrameOf(jsonValueAt(frame, `${rowPath}.allowed_frames[${frameIndex}]`));
            return parsed ?? fail(
              `${rowPath}.allowed_frames[${frameIndex}]`,
              'a cell, target or scene-resource pose frame',
            );
          });
        if (allowedFrames !== undefined && (type !== 'stored_pose' || allowedFrames.length === 0)) {
          return fail(`${rowPath}.allowed_frames`, 'a non-empty list on a stored_pose key');
        }
        return {
          key: stringAt(row.key, `${rowPath}.key`),
          type,
          access: access as 'read' | 'write' | 'read_write',
          persistence,
          ...(initial === undefined ? {} : { initial }),
          ...(expression === undefined ? {} : { expression }),
          ...(allowedFrames === undefined ? {} : { allowed_frames: allowedFrames }),
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
  if (record.execution_groups !== undefined) {
    const groupPath = `${path}.execution_groups`;
    const definitions = objectAt(record.execution_groups, groupPath);
    const names = Object.keys(definitions);
    if (names.length > 4096 || names.some((group) => (
      !/^[a-z][a-z0-9_.-]{0,127}$/.test(group)
      || group === 'all'
      || group === 'inherit'
    ))) return fail(groupPath, 'bounded portable non-reserved execution group IDs');
    if (names.some((group, index) => index > 0 && names[index - 1] >= group)) {
      return fail(groupPath, 'unique group IDs in canonical sorted order');
    }
    result.execution_groups = Object.fromEntries(names.map((name) => {
      const definitionPath = `${groupPath}.${name}`;
      const definition = objectAt(definitions[name], definitionPath);
      exactKeysAt(definition, ['execution_policy'], definitionPath);
      const policy = stringAt(
        definition.execution_policy,
        `${definitionPath}.execution_policy`,
      );
      if (![
        'independent',
        'common_start',
        'common_start_and_finish',
        'sync_offset_traj',
      ].includes(policy)) return fail(`${definitionPath}.execution_policy`, 'a supported policy');
      return [name, { execution_policy: policy as NonNullable<ProgramDocument['execution_groups']>[string]['execution_policy'] }];
    }));
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

export function parseProgramAuthoringDocument(
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
      'execution_groups',
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
