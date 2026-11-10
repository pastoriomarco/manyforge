import type { JsonValue } from '../types/api';

/**
 * The `io_signals` rows of one runtime unit configuration
 * (manyforge.value_schema.runtime.ros2_control_execution.v1, schema82
 * interface-freeze01, and runtime.io_provider_execution.v1 for an I/O device
 * component). Boolean inputs (Mock / ROS Bool / ros2_control GPIO /
 * geometric contact) and outputs (ROS SetBool / Mock / ros2_control GPIO) are
 * authored here. Any other row is kept verbatim. A machine's own I/O banks are
 * not authored here: they are I/O members whose signals come from their model.
 */

export type IoSourceProfile = 'mock' | 'ros_bool' | 'ros2_control_gpio' | 'geometric_contact';

export const IO_SOURCE_PROFILE_LABELS: Record<IoSourceProfile, string> = {
  mock: 'Mock',
  ros_bool: 'ROS Bool topic',
  ros2_control_gpio: 'ros2_control GPIO',
  geometric_contact: 'Geometric contact',
};

/**
 * Input liveness: Mock is in-process; a generic ROS Bool
 * topic declares periodic (freshness = a reading within the maximum sample
 * age) or change-only (liveness unproven between changes; refused in any
 * interrupt condition); ros2_control GPIO is periodic.
 */
export type IoLivenessMode = 'in_process' | 'periodic' | 'change_only';
export type RosBoolLiveness = 'periodic' | 'change_only';

export const IO_LIVENESS_LABELS: Record<IoLivenessMode, string> = {
  in_process: 'In process',
  periodic: 'Periodic',
  change_only: 'Change only',
};

export const IO_LIVENESS_UNPROVEN_WARNING =
  'Liveness unproven: this change-only source cannot prove it is alive between changes, so a reading may be old without showing stale. It cannot be used in an interrupt condition.';

/** Driver-reported freshness caveat of typed ros2_control inputs. */
export const IO_DRIVER_FRESHNESS_NOTE =
  'Freshness depends on the driver reporting read errors.';

export const IO_DEFAULT_INGRESS_DEPTH = 64;
export const IO_MAXIMUM_ROWS = 4096;
const MAXIMUM_SAFE = BigInt(Number.MAX_SAFE_INTEGER);
const PORTABLE_ID = /^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$/;
const NS_PER_MS = 1_000_000n;

export type IoOptionalProviderField =
  'ingress_depth' | 'initial_value';

export type IoOutputProfile = 'ros_set_bool' | 'mock_output' | 'ros2_control_gpio';

export const IO_OUTPUT_PROFILE_LABELS: Record<IoOutputProfile, string> = {
  ros_set_bool: 'ROS SetBool service',
  mock_output: 'Mock output',
  ros2_control_gpio: 'ros2_control GPIO command',
};

/** Limit of an authored ROS name or GPIO selector, in UTF-8 bytes. */
export const IO_MAXIMUM_SERVICE_NAME_BYTES = 4096;

/**
 * One editable boolean output: the shared row identity plus its
 * provider. An output has no freshness, edge, completion or readback field:
 * a ROS SetBool write completes on the service's correlated
 * acceptance, a ros2_control GPIO write is submitted-only, a
 * Mock write is immediate; readback is a separately declared input.
 */
export interface IoSignalOutputDraft {
  componentId: string;
  targetId: string;
  resourceRoleId: string;
  signalName: string;
  sourceId: string;
  profile: IoOutputProfile;
  /** Absolute, fully resolved ROS service name, authored exactly (no trim). */
  service: string;
  /** Mock output channel (portable id). */
  channel: string;
  /** ros2_control GPIO command topic (absolute, exact). */
  topic: string;
  group: string;
  interfaceName: string;
}

/** One editable boolean input declaration; numbers stay text until validated. */
export interface IoSignalInputDraft {
  componentId: string;
  targetId: string;
  resourceRoleId: string;
  signalName: string;
  sourceId: string;
  profile: IoSourceProfile;
  topic: string;
  /** ROS Bool only: the declared liveness (required by the schema). */
  liveness: RosBoolLiveness;
  /** ros2_control GPIO group and interface name (exact, no trim). */
  group: string;
  interfaceName: string;
  /** Geometric contact: selected robot component id. */
  robotComponentId: string;
  /** Geometric contact: an existing logical Scene resource. */
  targetSceneResourceId: string;
  /** Periodic sample-age freshness bound, in ms. */
  maximumAgeMs: string;
  ingressDepth: string;
  initialValue: boolean;
  echoEnabled: boolean;
  echoOutputSignalName: string;
  echoDelayMs: string;
  /** Optional provider fields authored explicitly in the source row. */
  explicit: ReadonlySet<IoOptionalProviderField>;
}

export type IoSignalRow =
  | { kind: 'input'; draft: IoSignalInputDraft }
  | { kind: 'output'; draft: IoSignalOutputDraft }
  /** A row this editor does not author (an unknown shape), kept unchanged. */
  | { kind: 'preserved'; raw: Record<string, JsonValue>; reason: string };

function object(value: JsonValue | undefined): Record<string, JsonValue> | null {
  return value !== null && typeof value === 'object' && !Array.isArray(value) ? value : null;
}

function safeInteger(value: JsonValue | undefined): number | null {
  return typeof value === 'number' && Number.isSafeInteger(value) ? value : null;
}

/** Exact ns → ms decimal text (no float rounding). */
export function nsToMsText(ns: number): string {
  const value = BigInt(ns);
  const whole = value / NS_PER_MS;
  const fraction = (value % NS_PER_MS).toString().padStart(6, '0').replace(/0+$/, '');
  return fraction === '' ? whole.toString() : `${whole}.${fraction}`;
}

/** Exact ms decimal text → integer ns, or null when not a nonnegative ns-exact value. */
export function msTextToNs(text: string): number | null {
  const match = /^(\d+)(?:\.(\d{1,6}))?$/.exec(text.trim());
  if (match === null) return null;
  const ns = BigInt(match[1]) * NS_PER_MS + BigInt((match[2] ?? '').padEnd(6, '0'));
  return ns <= MAXIMUM_SAFE ? Number(ns) : null;
}

function integerText(text: string): number | null {
  if (!/^\d+$/.test(text.trim())) return null;
  const value = BigInt(text.trim());
  return value <= MAXIMUM_SAFE ? Number(value) : null;
}

const INPUT_PROFILES: readonly IoSourceProfile[] = [
  'mock', 'ros_bool', 'ros2_control_gpio', 'geometric_contact',
];
const OUTPUT_PROFILES: readonly IoOutputProfile[] = ['ros_set_bool', 'mock_output', 'ros2_control_gpio'];

const PROVIDER_KEYS: Record<IoSourceProfile, string[]> = {
  mock: ['profile', 'ingress_depth', 'initial_value', 'echo'],
  ros_bool: ['profile', 'ingress_depth', 'topic', 'liveness'],
  ros2_control_gpio: ['profile', 'ingress_depth', 'topic', 'group', 'interface_name'],
  geometric_contact: [
    'profile', 'ingress_depth', 'robot_component_id', 'target_scene_resource_id',
  ],
};
const OUTPUT_PROVIDER_KEYS: Record<IoOutputProfile, string[]> = {
  ros_set_bool: ['profile', 'service'],
  mock_output: ['profile', 'channel'],
  ros2_control_gpio: ['profile', 'topic', 'group', 'interface_name'],
};
const ROW_KEYS = [
  'controllable_component_id', 'command_target_id', 'resource_role_id', 'signal_name',
  'source_id', 'value_type', 'direction', 'update_mode', 'transition_capability',
  'freshness', 'provider',
];

function exactKeys(record: Record<string, JsonValue>, keys: readonly string[], optional: readonly string[] = []): boolean {
  return Object.keys(record).every((key) => keys.includes(key))
    && keys.every((key) => optional.includes(key) || key in record);
}

/** The declared liveness of one ROS Bool provider, or null when not exactly `{mode}`. */
function rosBoolLiveness(provider: Record<string, JsonValue>): RosBoolLiveness | null {
  const liveness = object(provider.liveness);
  return liveness !== null && Object.keys(liveness).length === 1
    && (liveness.mode === 'periodic' || liveness.mode === 'change_only') ? liveness.mode : null;
}

function inputDraft(row: Record<string, JsonValue>): IoSignalInputDraft | null {
  const provider = object(row.provider);
  const freshness = object(row.freshness);
  const profile = provider?.profile as IoSourceProfile | undefined;
  if (
    provider === null || freshness === null || profile === undefined || !INPUT_PROFILES.includes(profile)
    || Object.keys(row).some((key) => !ROW_KEYS.includes(key))
    || !exactKeys(provider, PROVIDER_KEYS[profile], ['ingress_depth', 'initial_value', 'echo'])
    || row.value_type !== 'bool' || row.direction !== 'input'
    || row.transition_capability !== 'sampled_only'
  ) return null;
  const ids = [row.controllable_component_id, row.command_target_id, row.resource_role_id,
    row.signal_name, row.source_id];
  if (ids.some((value) => typeof value !== 'string')) return null;
  const liveness = profile === 'ros_bool' ? rosBoolLiveness(provider) : null;
  const echo = profile === 'mock' && provider.echo !== undefined ? object(provider.echo) : null;
  if (profile === 'mock' && provider.echo !== undefined && (echo === null
    || !exactKeys(echo, ['output_signal_name', 'delay_ms'])
    || typeof echo.output_signal_name !== 'string' || safeInteger(echo.delay_ms) === null)) return null;
  if (profile === 'ros_bool' && liveness === null) return null;
  // Mock and change-only ROS Bool: provider validity, no age; periodic sources
  // including geometric contact: sample age.
  const providerValidity = profile === 'mock' || liveness === 'change_only';
  const sampleAgeOk = freshness.mode === 'sample_age' && Object.keys(freshness).length === 2
    && (safeInteger(freshness.maximum_age_ns) ?? 0) > 0;
  const validityOk = freshness.mode === 'provider_validity' && Object.keys(freshness).length === 1;
  if (providerValidity ? !validityOk : !sampleAgeOk) return null;
  const expectedUpdate = profile === 'mock' ? row.update_mode === 'change_only'
    : profile === 'ros_bool' ? row.update_mode === (liveness === 'periodic' ? 'sampled' : 'change_only')
      : row.update_mode === 'sampled';
  if (!expectedUpdate) return null;
  if ((profile !== 'mock' && profile !== 'geometric_contact' && typeof provider.topic !== 'string')
    || (profile === 'geometric_contact' && typeof provider.robot_component_id !== 'string')
    || (profile === 'ros2_control_gpio'
      && (typeof provider.group !== 'string' || typeof provider.interface_name !== 'string'))
    || (profile === 'geometric_contact' && (
      typeof provider.target_scene_resource_id !== 'string'
    ))
  ) return null;
  const depth = provider.ingress_depth;
  if ((depth !== undefined && safeInteger(depth) === null)
    || (provider.initial_value !== undefined && typeof provider.initial_value !== 'boolean')
  ) return null;
  const explicit = new Set<IoOptionalProviderField>();
  for (const key of ['ingress_depth', 'initial_value'] as const) {
    if (provider[key] !== undefined) explicit.add(key);
  }
  return {
    componentId: row.controllable_component_id as string,
    targetId: row.command_target_id as string,
    resourceRoleId: row.resource_role_id as string,
    signalName: row.signal_name as string,
    sourceId: row.source_id as string,
    profile,
    topic: typeof provider.topic === 'string' ? provider.topic : '',
    liveness: liveness ?? 'periodic',
    group: typeof provider.group === 'string' ? provider.group : '',
    interfaceName: typeof provider.interface_name === 'string' ? provider.interface_name : '',
    targetSceneResourceId: typeof provider.target_scene_resource_id === 'string'
      ? provider.target_scene_resource_id : '',
    robotComponentId: typeof provider.robot_component_id === 'string' ? provider.robot_component_id : '',
    maximumAgeMs: sampleAgeOk ? nsToMsText(freshness.maximum_age_ns as number) : '',
    ingressDepth: String(safeInteger(depth) ?? IO_DEFAULT_INGRESS_DEPTH),
    initialValue: provider.initial_value === true,
    echoEnabled: echo !== null,
    echoOutputSignalName: typeof echo?.output_signal_name === 'string' ? echo.output_signal_name : '',
    echoDelayMs: safeInteger(echo?.delay_ms)?.toString() ?? '0',
    explicit,
  };
}

const OUTPUT_ROW_KEYS = [
  'controllable_component_id', 'command_target_id', 'resource_role_id', 'signal_name',
  'source_id', 'value_type', 'direction', 'provider',
];

function outputDraft(row: Record<string, JsonValue>): IoSignalOutputDraft | null {
  const provider = object(row.provider);
  const profile = provider?.profile as IoOutputProfile | undefined;
  if (
    provider === null || profile === undefined || !OUTPUT_PROFILES.includes(profile)
    || !exactKeys(provider, OUTPUT_PROVIDER_KEYS[profile])
    || OUTPUT_PROVIDER_KEYS[profile].some((key) => key !== 'profile' && typeof provider[key] !== 'string')
    || !exactKeys(row, OUTPUT_ROW_KEYS)
    || row.value_type !== 'bool' || row.direction !== 'output'
  ) return null;
  const ids = [row.controllable_component_id, row.command_target_id, row.resource_role_id,
    row.signal_name, row.source_id];
  if (ids.some((value) => typeof value !== 'string')) return null;
  const text = (key: string) => (typeof provider[key] === 'string' ? provider[key] as string : '');
  return {
    componentId: row.controllable_component_id as string,
    targetId: row.command_target_id as string,
    resourceRoleId: row.resource_role_id as string,
    signalName: row.signal_name as string,
    sourceId: row.source_id as string,
    profile,
    service: text('service'),
    channel: text('channel'),
    topic: text('topic'),
    group: text('group'),
    interfaceName: text('interface_name'),
  };
}

/**
 * The problem with one authored absolute ROS name (`kind` names it in the
 * message), or null: non-empty, no NUL, at most 4096 bytes, absolute and
 * fully resolved (no relative, private `~` or `{substitution}` form), with no
 * repeated or trailing slash.
 */
export function rosNameProblem(name: string, kind = 'ROS service name'): string | null {
  if (name === '') return `enter the ${kind}.`;
  if (name.includes('\0')) return `the ${kind} must not contain NUL.`;
  if (new TextEncoder().encode(name).length > IO_MAXIMUM_SERVICE_NAME_BYTES) {
    return `the ${kind} must be at most ${IO_MAXIMUM_SERVICE_NAME_BYTES} bytes.`;
  }
  if (!name.startsWith('/')) return `the ${kind} must be absolute (start with /).`;
  if (/[~{}]/.test(name)) return `the ${kind} must be fully resolved (no ~ or {substitution}).`;
  if (name.includes('//') || name.endsWith('/')) {
    return `the ${kind} must not have a repeated or trailing slash.`;
  }
  return null;
}

export function rosServiceNameProblem(service: string): string | null {
  return rosNameProblem(service);
}

/** A ros2_control GPIO group / interface name: exact, non-empty, NUL-free, ≤ 4096 bytes. */
function selectorProblem(value: string, kind: string): string | null {
  if (value === '') return `enter the ${kind}.`;
  if (value.includes('\0')) return `the ${kind} must not contain NUL.`;
  return new TextEncoder().encode(value).length > IO_MAXIMUM_SERVICE_NAME_BYTES
    ? `the ${kind} must be at most ${IO_MAXIMUM_SERVICE_NAME_BYTES} bytes.` : null;
}

function gpioProblem(draft: { topic: string; group: string; interfaceName: string }): string | null {
  return rosNameProblem(draft.topic, 'GPIO topic')
    ?? selectorProblem(draft.group, 'GPIO group')
    ?? selectorProblem(draft.interfaceName, 'GPIO interface name');
}

function preservedReason(row: Record<string, JsonValue>): string {
  return row.direction === 'output'
    ? 'Output: not a ROS SetBool, ros2_control GPIO or Mock output shape; kept unchanged.'
    : 'Not an executable boolean signal shape; kept unchanged.';
}

/** Every declared row of one configuration, in authored order. */
export function ioSignalRows(configuration: Record<string, JsonValue> | null): IoSignalRow[] {
  const rows = configuration?.io_signals;
  if (!Array.isArray(rows)) return [];
  return rows.flatMap((value): IoSignalRow[] => {
    const row = object(value);
    if (row === null) return [];
    const draft = inputDraft(row);
    if (draft !== null) return [{ kind: 'input', draft }];
    const output = outputDraft(row);
    if (output !== null) return [{ kind: 'output', draft: output }];
    return [{ kind: 'preserved', raw: row, reason: preservedReason(row) }];
  });
}

export function newIoSignalInput(componentId = ''): IoSignalInputDraft {
  return {
    componentId,
    targetId: '',
    resourceRoleId: '',
    signalName: '',
    sourceId: '',
    profile: 'mock',
    topic: '',
    liveness: 'periodic',
    group: '',
    interfaceName: '',
    robotComponentId: '',
    targetSceneResourceId: '',
    maximumAgeMs: '',
    ingressDepth: String(IO_DEFAULT_INGRESS_DEPTH),
    initialValue: false,
    echoEnabled: false,
    echoOutputSignalName: '',
    echoDelayMs: '0',
    explicit: new Set(),
  };
}

/** Switch a row's direction, keeping its shared identity fields. */
export function withIoDirection(
  row: Extract<IoSignalRow, { kind: 'input' | 'output' }>,
  direction: 'input' | 'output',
): IoSignalRow {
  if (row.kind === direction) return row;
  const { componentId, targetId, resourceRoleId, signalName, sourceId } = row.draft;
  const shared = { componentId, targetId, resourceRoleId, signalName, sourceId };
  return direction === 'output'
    ? {
      kind: 'output',
      draft: { ...shared, profile: 'ros_set_bool', service: '', channel: '', topic: '', group: '', interfaceName: '' },
    }
    : { kind: 'input', draft: { ...newIoSignalInput(), ...shared } };
}

/** The liveness an input's source kind declares. */
export function inputLivenessMode(draft: Pick<IoSignalInputDraft, 'profile' | 'liveness'>): IoLivenessMode {
  return draft.profile === 'mock' ? 'in_process'
    : draft.profile === 'ros_bool' ? draft.liveness : 'periodic';
}

/** Whether an input uses the periodic sample-age freshness budget. */
export function inputUsesSampleAge(draft: Pick<IoSignalInputDraft, 'profile' | 'liveness'>): boolean {
  return inputLivenessMode(draft) === 'periodic';
}

/** The declared liveness of one raw `io_signals` input row, or null when not derivable. */
export function rowInputLiveness(row: Record<string, JsonValue> | null): IoLivenessMode | null {
  if (row?.direction !== 'input') return null;
  const provider = object(row.provider);
  const profile = provider?.profile;
  if (profile === 'mock') return 'in_process';
  if (profile === 'ros2_control_gpio' || profile === 'geometric_contact') return 'periodic';
  return profile === 'ros_bool' && provider !== null ? rosBoolLiveness(provider) : null;
}

function rowLabel(draft: { signalName: string }, index: number): string {
  return draft.signalName.trim() === '' ? `Signal ${index + 1}` : draft.signalName;
}

function identityProblem(draft: IoSignalInputDraft | IoSignalOutputDraft, label: string): string | null {
  for (const [name, value] of [
    ['component', draft.componentId], ['target', draft.targetId],
    ['resource role', draft.resourceRoleId], ['signal name', draft.signalName],
    ['source', draft.sourceId],
  ] as const) {
    if (!PORTABLE_ID.test(value)) {
      return `${label}: the ${name} must be 1–128 letters, digits, '.', '_' or '-', starting with a letter or digit.`;
    }
  }
  return null;
}

/** The first validation problem of one output, or null. */
export function validateIoSignalOutput(draft: IoSignalOutputDraft, index = 0): string | null {
  const label = rowLabel(draft, index);
  const identity = identityProblem(draft, label);
  if (identity !== null) return identity;
  if (draft.profile === 'mock_output') {
    return PORTABLE_ID.test(draft.channel) ? null
      : `${label}: the Mock output channel must be 1–128 letters, digits, '.', '_' or '-', starting with a letter or digit.`;
  }
  const problem = draft.profile === 'ros2_control_gpio' ? gpioProblem(draft)
    : rosNameProblem(draft.service);
  return problem === null ? null : `${label}: ${problem}`;
}

/** The first validation problem of one input, or null. */
export function validateIoSignalInput(draft: IoSignalInputDraft, index = 0): string | null {
  const label = rowLabel(draft, index);
  const identity = identityProblem(draft, label);
  if (identity !== null) return identity;
  const depth = integerText(draft.ingressDepth);
  if (depth === null || depth < 1) {
    return `${label}: ingress depth must be a whole number of samples from 1 to ${Number.MAX_SAFE_INTEGER}.`;
  }
  if (draft.profile === 'mock') {
    if (!draft.echoEnabled) return null;
    const delay = integerText(draft.echoDelayMs);
    if (!PORTABLE_ID.test(draft.echoOutputSignalName)) return `${label}: enter a Mock output signal name.`;
    return delay === null || delay > 9_007_199_254
      ? `${label}: the Mock echo delay must be a whole number of milliseconds from 0 to 9007199254.` : null;
  }
  if (draft.profile === 'geometric_contact') {
    if (!PORTABLE_ID.test(draft.robotComponentId)) {
      return `${label}: select a robot.`;
    }
    if (!PORTABLE_ID.test(draft.targetSceneResourceId)) {
      return `${label}: enter a target Scene resource id.`;
    }
    const maximumAge = msTextToNs(draft.maximumAgeMs);
    if (maximumAge === null || maximumAge < 1) {
      return `${label}: the maximum sample age must be above 0 ms, with at most 6 decimals.`;
    }
    return null;
  }
  const address = draft.profile === 'ros_bool'
    ? (draft.topic.trim() === '' ? 'enter the source topic.' : null)
    : gpioProblem(draft);
  if (address !== null) return `${label}: ${address}`;
  if (inputUsesSampleAge(draft)) {
    const age = msTextToNs(draft.maximumAgeMs);
    if (age === null || age < 1) {
      return `${label}: the maximum sample age must be above 0 ms, with at most 6 decimals.`;
    }
  }
  return null;
}

function keyOf(row: IoSignalRow): string | null {
  if (row.kind !== 'preserved') {
    return JSON.stringify([row.draft.componentId, row.draft.targetId, row.draft.resourceRoleId]);
  }
  const { controllable_component_id: c, command_target_id: t, resource_role_id: r } = row.raw;
  return typeof c === 'string' && typeof t === 'string' && typeof r === 'string'
    ? JSON.stringify([c, t, r]) : null;
}

/**
 * The I/O device rules of one io_provider component's rows: at least one
 * signal, every row owned by the device itself.
 */
export function ioProviderRowsProblem(rows: IoSignalRow[], componentId: string): string | null {
  if (rows.length === 0) return 'An I/O device needs at least one signal; remove the device instead.';
  for (const [index, row] of rows.entries()) {
    const { owner, label } = row.kind === 'preserved'
      ? {
        owner: row.raw.controllable_component_id,
        label: String(row.raw.signal_name ?? `Signal ${index + 1}`),
      }
      : { owner: row.draft.componentId, label: rowLabel(row.draft, index) };
    if (owner !== componentId) return `${label}: a signal of I/O device ${componentId} must belong to ${componentId}.`;
  }
  return null;
}

/** What rows sharing one source must agree on (profile, address, depth, liveness). */
function sourceFacts(row: Extract<IoSignalRow, { kind: 'input' | 'output' }>): string {
  if (row.kind === 'output') {
    const { draft } = row;
    return JSON.stringify(['output', draft.profile,
      draft.profile === 'ros_set_bool' ? draft.service
        : draft.profile === 'ros2_control_gpio' ? draft.topic : null]);
  }
  const { draft } = row;
  if (draft.profile === 'geometric_contact') {
    return JSON.stringify(['input', draft.profile, integerText(draft.ingressDepth),
      draft.robotComponentId,
      draft.targetSceneResourceId, inputLivenessMode(draft)]);
  }
  return JSON.stringify(['input', draft.profile,
    draft.profile === 'mock' ? null : draft.profile === 'ros_bool' ? draft.topic.trim() : draft.topic,
    integerText(draft.ingressDepth), inputLivenessMode(draft)]);
}

/**
 * Validate every row and the cross-row rules; the first problem, or null.
 * `ioProviderComponentId` adds the I/O device rules for that component.
 */
export function validateIoSignalRows(rows: IoSignalRow[], ioProviderComponentId?: string): string | null {
  if (rows.length > IO_MAXIMUM_ROWS) return `At most ${IO_MAXIMUM_ROWS} signals per runtime unit.`;
  if (ioProviderComponentId !== undefined) {
    const problem = ioProviderRowsProblem(rows, ioProviderComponentId);
    if (problem !== null) return problem;
  }
  for (const [index, row] of rows.entries()) {
    if (row.kind === 'preserved') continue;
    const problem = row.kind === 'input'
      ? validateIoSignalInput(row.draft, index)
      : validateIoSignalOutput(row.draft, index);
    if (problem !== null) return problem;
  }
  const keys = new Set<string>();
  const names = new Set<string>();
  const sources = new Map<string, string>();
  for (const row of rows) {
    const key = keyOf(row);
    if (key !== null) {
      if (keys.has(key)) {
        return 'Each component, target and resource role may declare only one signal.';
      }
      keys.add(key);
    }
    const component = row.kind === 'preserved' ? row.raw.controllable_component_id : row.draft.componentId;
    const name = row.kind === 'preserved' ? row.raw.signal_name : row.draft.signalName;
    if (typeof component === 'string' && typeof name === 'string') {
      const scoped = JSON.stringify([component, name]);
      if (names.has(scoped)) return `Signal name ${name} is used twice in component ${component}.`;
      names.add(scoped);
    }
    if (row.kind === 'preserved') continue;
    const facts = sourceFacts(row);
    const existing = sources.get(row.draft.sourceId);
    if (existing !== undefined && existing !== facts) {
      return `Signals sharing source ${row.draft.sourceId} must use the same direction, source profile, topic or service, ingress depth and liveness.`;
    }
    sources.set(row.draft.sourceId, facts);
  }
  for (const row of rows) {
    if (row.kind !== 'input' || row.draft.profile !== 'mock' || !row.draft.echoEnabled) continue;
    if (!rows.some((candidate) => candidate.kind === 'output'
      && candidate.draft.componentId === row.draft.componentId
      && candidate.draft.signalName === row.draft.echoOutputSignalName
      && candidate.draft.profile === 'mock_output')) {
      return `${row.draft.signalName}: the echo must name a Mock output on this I/O device.`;
    }
  }
  return null;
}

/** The exact schema row of one valid input. */
export function ioSignalInputValue(draft: IoSignalInputDraft): Record<string, JsonValue> {
  const depth = integerText(draft.ingressDepth)!;
  const optional = (
    key: IoOptionalProviderField,
    value: JsonValue,
    fallback: JsonValue,
  ): Record<string, JsonValue> => (
    draft.explicit.has(key) || value !== fallback ? { [key]: value } : {}
  );
  const base = {
    controllable_component_id: draft.componentId,
    command_target_id: draft.targetId,
    resource_role_id: draft.resourceRoleId,
    signal_name: draft.signalName,
    source_id: draft.sourceId,
    value_type: 'bool',
    direction: 'input',
    transition_capability: 'sampled_only',
  };
  if (draft.profile === 'mock') {
    return {
      ...base,
      update_mode: 'change_only',
      freshness: { mode: 'provider_validity' },
      provider: {
        profile: 'mock',
        ...optional('ingress_depth', depth, IO_DEFAULT_INGRESS_DEPTH),
        ...optional('initial_value', draft.initialValue, false),
        ...(draft.echoEnabled ? { echo: { output_signal_name: draft.echoOutputSignalName,
          delay_ms: integerText(draft.echoDelayMs)! } } : {}),
      },
    };
  }
  if (draft.profile === 'geometric_contact') {
    return {
      ...base,
      update_mode: 'sampled',
      freshness: { mode: 'sample_age', maximum_age_ns: msTextToNs(draft.maximumAgeMs)! },
      provider: {
        profile: draft.profile,
        ...optional('ingress_depth', depth, IO_DEFAULT_INGRESS_DEPTH),
        robot_component_id: draft.robotComponentId,
        target_scene_resource_id: draft.targetSceneResourceId,
      },
    };
  }
  const periodic = inputUsesSampleAge(draft);
  const address: Record<string, JsonValue> = draft.profile === 'ros_bool'
    ? { topic: draft.topic.trim(), liveness: { mode: draft.liveness } }
    : { topic: draft.topic, group: draft.group, interface_name: draft.interfaceName };
  return {
    ...base,
    update_mode: periodic ? 'sampled' : 'change_only',
    freshness: periodic
      ? { mode: 'sample_age', maximum_age_ns: msTextToNs(draft.maximumAgeMs)! }
      : { mode: 'provider_validity' },
    provider: {
      profile: draft.profile,
      ...address,
      ...optional('ingress_depth', depth, IO_DEFAULT_INGRESS_DEPTH),
    },
  };
}

/** The exact schema row of one valid output: no freshness/edge/completion/readback. */
export function ioSignalOutputValue(draft: IoSignalOutputDraft): Record<string, JsonValue> {
  const provider: Record<string, JsonValue> = draft.profile === 'mock_output'
    ? { profile: draft.profile, channel: draft.channel }
    : draft.profile === 'ros_set_bool' ? { profile: draft.profile, service: draft.service }
      : { profile: draft.profile, topic: draft.topic, group: draft.group, interface_name: draft.interfaceName };
  return {
    controllable_component_id: draft.componentId,
    command_target_id: draft.targetId,
    resource_role_id: draft.resourceRoleId,
    signal_name: draft.signalName,
    source_id: draft.sourceId,
    value_type: 'bool',
    direction: 'output',
    provider,
  };
}

/** The complete `io_signals` array to commit (rows must be valid). */
export function ioSignalRowsValue(rows: IoSignalRow[]): JsonValue[] {
  return rows.map((row) => (
    row.kind === 'input' ? ioSignalInputValue(row.draft)
      : row.kind === 'output' ? ioSignalOutputValue(row.draft) : row.raw
  ));
}

/** Where a runtime configuration keeps its `io_signals` (intent v2 nests it). */
export function ioSignalsConfiguration(
  configuration: Record<string, JsonValue>,
  configurationPath: Array<string | number>,
): { configuration: Record<string, JsonValue>; path: Array<string | number> } {
  const nested = object(configuration.runtime_configuration);
  return nested !== null
    ? { configuration: nested, path: [...configurationPath, 'runtime_configuration', 'io_signals'] }
    : { configuration, path: [...configurationPath, 'io_signals'] };
}

/** The declared source facts a GUI shows for one bound signal. */
export interface DeclaredIoSource {
  profile: IoSourceProfile;
  sourceId: string;
  topic: string | null;
  liveness: IoLivenessMode;
}

/** Every declared row of a Deployment document (intent components and compiled runtime units). */
function declaredIoRows(document: Record<string, JsonValue> | null): IoSignalRow[] {
  const configurations: Record<string, JsonValue>[] = [];
  const components = document?.components;
  if (Array.isArray(components)) {
    for (const component of components) {
      const configuration = object(object(component)?.configuration);
      if (configuration !== null) configurations.push(configuration);
    }
  }
  const units = object(document?.graph)?.runtime_units;
  if (Array.isArray(units)) {
    for (const unit of units) {
      const configuration = object(object(unit)?.configuration);
      if (configuration !== null) configurations.push(configuration);
    }
  }
  return configurations.flatMap((configuration) => ioSignalRows(
    ioSignalsConfiguration(configuration, []).configuration,
  ));
}

/** Every declared input row of a Deployment document (intent components and compiled runtime units). */
export function declaredIoInputs(document: Record<string, JsonValue> | null): IoSignalInputDraft[] {
  return declaredIoRows(document).flatMap((row) => (row.kind === 'input' ? [row.draft] : []));
}

/** Every declared output row of a Deployment document (intent components and compiled runtime units). */
export function declaredIoOutputs(document: Record<string, JsonValue> | null): IoSignalOutputDraft[] {
  return declaredIoRows(document).flatMap((row) => (row.kind === 'output' ? [row.draft] : []));
}

/** Every declared input of a Deployment document, by component/target/signal name. */
export function declaredIoSources(
  document: Record<string, JsonValue> | null,
): Map<string, DeclaredIoSource> {
  const result = new Map<string, DeclaredIoSource>();
  for (const draft of declaredIoInputs(document)) {
    result.set(ioSourceKey(draft.componentId, draft.targetId, draft.signalName), {
      profile: draft.profile,
      sourceId: draft.sourceId,
      topic: draft.profile === 'mock' || draft.profile === 'geometric_contact' ? null : draft.topic,
      liveness: inputLivenessMode(draft),
    });
  }
  return result;
}

/** Where one declared input comes from, for operator text (e.g. `ROS topic /door`). */
export function ioInputSourceText(draft: IoSignalInputDraft): string {
  switch (draft.profile) {
    case 'ros_bool': return `ROS topic ${draft.topic}`;
    case 'ros2_control_gpio': return `ROS topic ${draft.topic}, GPIO ${draft.group}/${draft.interfaceName}`;
    case 'geometric_contact': return `geometric contact for ${draft.robotComponentId} with ${draft.targetSceneResourceId}`;
    case 'mock': return 'Mock input';
  }
}

/** The declared source text of each input of a Deployment document, by component, signal name and source id. */
export function declaredIoInputSourceTexts(document: Record<string, JsonValue> | null): Map<string, string> {
  return new Map(declaredIoInputs(document).map((draft) => [
    JSON.stringify([draft.componentId, draft.signalName, draft.sourceId]), ioInputSourceText(draft),
  ]));
}

export function ioSourceKey(componentId: string, targetId: string, signalName: string): string {
  return JSON.stringify([componentId, targetId, signalName]);
}

/* ---- I/O device components (component_profile_id io_device_v0) ---- */

export const IO_DEVICE_PROFILE_ID = 'io_device_v0';
export const IO_PROVIDER_RUNTIME_KIND = 'io_provider';
/** Per-device lifecycle command time limit, prefilled with the
 * default component lifecycle budget (10 s, as every component). */
export const IO_DEVICE_DEFAULT_LIFECYCLE_TIMEOUT_S = 10;
/** The existing lifecycle transaction maximum (kMaxLifecycleTransactionTimeoutNs). */
export const IO_DEVICE_MAXIMUM_LIFECYCLE_TIMEOUT_S = 3600;
/** What the lifecycle command time limit bounds (every component kind). */
export const LIFECYCLE_TIMEOUT_HELP = `Prepare, Enable and Disable of this component must each finish within this time (above 0, at most ${IO_DEVICE_MAXIMUM_LIFECYCLE_TIMEOUT_S} s); a step that doesn't finish in time ends with an unknown outcome and isn't resent.`;
const COMPONENT_ID = /^[a-z][a-z0-9._-]{0,127}$/;
const MAXIMUM_COMPONENTS = 128;

/** Whether one Deployment intent component is a model-free I/O device. */
export function isIoProviderComponent(component: JsonValue | undefined): boolean {
  return object(object(component)?.configuration)?.runtime_kind === IO_PROVIDER_RUNTIME_KIND;
}

/** The problem with one authored lifecycle time limit (seconds text), or null. */
export function lifecycleTimeoutProblem(text: string): string | null {
  const trimmed = text.trim();
  const value = Number(trimmed);
  return /^\d+(\.\d+)?$/.test(trimmed) && value > 0 && value <= IO_DEVICE_MAXIMUM_LIFECYCLE_TIMEOUT_S
    ? null
    : `The lifecycle command time limit must be above 0 s and at most ${IO_DEVICE_MAXIMUM_LIFECYCLE_TIMEOUT_S} s.`;
}

/**
 * Provisional prefill of the device output acknowledgement timeout (contracts
 * 65, 68, 82): it covers ROS SetBool outputs, to be measured on
 * the live/real lanes. ros2_control GPIO outputs are submitted-only and Mock
 * outputs immediate: neither has an acknowledgement budget.
 */
export const IO_DEVICE_PROVISIONAL_OUTPUT_ACK_TIMEOUT_S = 0.5;

/** Whether a device's runtime configuration has an acknowledged ROS service output. */
export function ioDeviceHasAcknowledgedOutput(runtimeConfiguration: Record<string, JsonValue> | null): boolean {
  const rows = runtimeConfiguration?.io_signals;
  return Array.isArray(rows) && rows.some((value) => {
    const row = object(value);
    const profile = object(row?.provider)?.profile;
    return row?.direction === 'output' && profile === 'ros_set_bool';
  });
}

/** Whether a device's runtime configuration has a submitted-only ros2_control GPIO output. */
export function ioDeviceHasGpioOutput(runtimeConfiguration: Record<string, JsonValue> | null): boolean {
  const rows = runtimeConfiguration?.io_signals;
  return Array.isArray(rows) && rows.some((value) => {
    const row = object(value);
    return row?.direction === 'output' && object(row.provider)?.profile === 'ros2_control_gpio';
  });
}

/** The problem with one authored output acknowledgement timeout (seconds text), or null. */
export function outputAckTimeoutProblem(text: string): string | null {
  const trimmed = text.trim();
  const value = Number(trimmed);
  return /^\d+(\.\d+)?$/.test(trimmed) && Number.isFinite(value) && value > 0
    ? null
    : 'The output acknowledgement timeout must be a finite number of seconds above 0.';
}

/**
 * The one `io_provider_devices` row of an I/O device. The output
 * acknowledgement timeout is carried only when given (only devices with ROS
 * SetBool outputs may carry it).
 */
export function ioProviderDeviceValue(
  componentId: string,
  timeoutS: number,
  outputAckTimeoutS?: number,
): Record<string, JsonValue> {
  return {
    controllable_component_id: componentId,
    lifecycle_command_timeout_s: timeoutS,
    ...(outputAckTimeoutS === undefined ? {} : { output_source_ack_timeout_s: outputAckTimeoutS }),
  };
}

const DEVICE_ROW_KEYS = new Set(['controllable_component_id', 'lifecycle_command_timeout_s', 'output_source_ack_timeout_s']);

/**
 * The authored lifecycle time limit and output acknowledgement timeout of one
 * I/O device, or the materialized defaults when its row or value is absent.
 * `exact` is false when the rows are not exactly one row for the device (a
 * commit rewrites them to that row).
 */
export function ioProviderDeviceTimeout(
  runtimeConfiguration: Record<string, JsonValue> | null,
  componentId: string,
): { timeoutS: number; authored: boolean; exact: boolean; outputAckTimeoutS: number | null } {
  const rows = runtimeConfiguration?.io_provider_devices;
  const only = Array.isArray(rows) && rows.length === 1 ? object(rows[0]) : null;
  const own = only !== null && only.controllable_component_id === componentId
    && Object.keys(only).every((key) => DEVICE_ROW_KEYS.has(key));
  const value = own ? only?.lifecycle_command_timeout_s : undefined;
  const ack = own ? only?.output_source_ack_timeout_s : undefined;
  const authored = typeof value === 'number' && Number.isFinite(value);
  const ackAuthored = typeof ack === 'number' && Number.isFinite(ack);
  return {
    timeoutS: authored ? value : IO_DEVICE_DEFAULT_LIFECYCLE_TIMEOUT_S,
    authored,
    exact: own && (value === undefined || authored) && (ack === undefined || ackAuthored),
    outputAckTimeoutS: ackAuthored ? ack : null,
  };
}

/** The editable Add-I/O-device form. */
export interface IoDeviceDraft {
  componentId: string;
  lifecycleTimeoutS: string;
  signal: Extract<IoSignalRow, { kind: 'input' | 'output' }>;
}

/** A new form: the first free `io_device_N`, 10 s, one Mock input on target `io`. */
export function newIoDeviceDraft(existingComponentIds: readonly string[]): IoDeviceDraft {
  let number = 1;
  while (existingComponentIds.includes(`io_device_${number}`)) number += 1;
  const componentId = `io_device_${number}`;
  return {
    componentId,
    lifecycleTimeoutS: String(IO_DEVICE_DEFAULT_LIFECYCLE_TIMEOUT_S),
    signal: { kind: 'input', draft: { ...newIoSignalInput(componentId), targetId: 'io' } },
  };
}

/**
 * The first signal as committed: owned by the device; a blank resource role
 * uses the signal name and a blank source `<component id>_source`.
 */
export function ioDeviceSignalRow(draft: IoDeviceDraft): Extract<IoSignalRow, { kind: 'input' | 'output' }> {
  const signal = draft.signal.draft;
  const shared = {
    componentId: draft.componentId,
    resourceRoleId: signal.resourceRoleId.trim() === '' ? signal.signalName : signal.resourceRoleId,
    sourceId: signal.sourceId.trim() === '' ? `${draft.componentId}_source` : signal.sourceId,
  };
  return draft.signal.kind === 'input'
    ? { kind: 'input', draft: { ...draft.signal.draft, ...shared } }
    : { kind: 'output', draft: { ...draft.signal.draft, ...shared } };
}

/** The first problem of an Add-I/O-device form, or null. */
export function validateIoDeviceDraft(
  draft: IoDeviceDraft,
  existingComponentIds: readonly string[],
): string | null {
  if (!COMPONENT_ID.test(draft.componentId)) {
    return "The component id must be 1–128 lowercase letters, digits, '.', '_' or '-', starting with a letter.";
  }
  if (existingComponentIds.includes(draft.componentId)) return `Component ${draft.componentId} already exists.`;
  if (existingComponentIds.length >= MAXIMUM_COMPONENTS) return `At most ${MAXIMUM_COMPONENTS} components per Deployment.`;
  if (draft.signal.draft.signalName.trim() === '') return 'Enter the first signal name.';
  return lifecycleTimeoutProblem(draft.lifecycleTimeoutS)
    ?? validateIoSignalRows([ioDeviceSignalRow(draft)], draft.componentId);
}

/**
 * The exact `components[]` row of a valid form: model-free (no model_source,
 * root_pose_in_cell, frame_prefix or Stop fields), empty target settings.
 */
export function ioDeviceComponentValue(draft: IoDeviceDraft): Record<string, JsonValue> {
  return {
    component_id: draft.componentId,
    component_profile_id: IO_DEVICE_PROFILE_ID,
    target_settings: {},
    configuration: {
      runtime_kind: IO_PROVIDER_RUNTIME_KIND,
      runtime_configuration: {
        io_provider_devices: [ioProviderDeviceValue(draft.componentId, Number(draft.lifecycleTimeoutS.trim()))],
        io_signals: ioSignalRowsValue([ioDeviceSignalRow(draft)]),
      },
    },
  };
}
