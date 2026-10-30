import type {
  JsonValue,
  ManagerDeploymentCandidatePreviewResponse,
  ManagerDeploymentImpactComponent,
  SourceModelTarget,
} from '../types/api';
import {
  IO_OUTPUT_PROFILE_LABELS,
  declaredIoInputs,
  declaredIoOutputs,
  ioInputSourceText,
} from './ioSignalConfiguration';
import type {
  ComponentCatalogEntry,
  ComponentCatalogKind,
  ComponentProfileFacts,
} from '../types/componentCatalog';
import { draftAttachments, fromDraft } from '../state/kinematicIndex';

/**
 * Pure helpers of the Components section (one page per component) and of the
 * Deployment tabs (Overview, Roles, Cell settings, Changes). The Deployment
 * Draft, Manager Applied and the Manager preview stay the authorities; these
 * only present them and prepare an Add component edit.
 */

export type ComponentManagerTab = 'overview' | 'roles' | 'cell' | 'changes';

/** One component of the Draft or of Manager Applied. */
export interface ComponentManagerRow {
  componentId: string;
  profileId: string | null;
  /** Null when only Manager Applied names the component. */
  kind: ComponentCatalogKind | null;
  /** A mounted tool's parent interface, `<robot>.<interface>`; null otherwise. */
  mount: string | null;
  /** `applied`: in both; `added`: Draft only (runs after Apply); `removed`: Applied only. */
  change: 'applied' | 'added' | 'removed';
}

export interface RoleFeedback {
  roleId: string;
  componentId: string | null;
  requiredTags: string[];
  /** `unknown`: the role requires type tags and the component's profile has no known tags. */
  status: 'satisfied' | 'unbound' | 'incompatible' | 'unknown';
  /** Satisfied by a component that runs only after the next Apply. */
  pendingApply: boolean;
  detail: string;
}

export const COMPONENT_KIND_LABELS: Record<ComponentCatalogKind, string> = {
  robot: 'Robot',
  tool: 'Tool',
  io_device: 'I/O device',
  pose_source: 'Pose source',
  ai_model: 'AI model',
};

const MODEL_FREE_KINDS: Record<string, ComponentCatalogKind> = {
  io_provider: 'io_device',
  pose_source: 'pose_source',
  ai_model: 'ai_model',
};

/** The Deployment intent's component id rule (`deployment_intent.py`). */
const COMPONENT_ID = /^[a-z][a-z0-9._-]{0,127}$/;

function record(value: JsonValue | undefined): Record<string, JsonValue> | null {
  return value !== null && value !== undefined && typeof value === 'object' && !Array.isArray(value)
    ? value : null;
}

function strings(value: JsonValue | undefined): string[] {
  return Array.isArray(value) ? value.filter((item): item is string => typeof item === 'string') : [];
}

/** A model row is a tool when its component type is tagged `tool`, else a robot. */
export function componentKindOf(
  row: Record<string, JsonValue>,
  facts?: ComponentProfileFacts,
): ComponentCatalogKind {
  const runtimeKind = record(row.configuration)?.runtime_kind;
  return (typeof runtimeKind === 'string' ? MODEL_FREE_KINDS[runtimeKind] : undefined)
    ?? (facts?.tags.includes('tool') ? 'tool' : 'robot');
}

/** The tool mounted on a component's `flange` in the Draft, or null. */
export function toolOnFlange(document: Record<string, JsonValue> | null, componentId: string): string | null {
  return fromDraft(document).toolOn(componentId, 'flange');
}

/**
 * Draft components a tool can mount on: their component type has a `flange`
 * attachment interface, and no tool is mounted on it yet (one per flange).
 */
export function mountCandidates(
  document: Record<string, JsonValue> | null,
  profiles: Readonly<Record<string, ComponentProfileFacts>>,
): string[] {
  const index = fromDraft(document);
  return draftComponents(document).flatMap((row) => {
    const id = row.component_id as string;
    const profile = typeof row.component_profile_id === 'string' ? profiles[row.component_profile_id] : undefined;
    return profile?.attachmentInterfaces.includes('flange') && index.toolOn(id, 'flange') === null ? [id] : [];
  }).sort();
}

/** The attachment row that mounts a tool on a robot's flange (identity pose). */
export function toolMountRow(robotId: string, toolId: string): Record<string, JsonValue> {
  return {
    parent: { component_id: robotId, interface: 'flange' },
    child: { component_id: toolId, interface: 'mount' },
  };
}

/**
 * What a robot's `tcp` frame role resolves to (D1): the default TCP of the
 * tool mounted on its flange in the Draft, else the robot's own `tcp`, else
 * its `flange`. `appliedFrames` are the Applied frames of the selected target
 * (source model); without a mounted tool, the frame names come from there.
 * Null for a target that is not a robot motion target (no `base` frame).
 */
export function resolvedRobotTcp(
  document: Record<string, JsonValue> | null,
  componentId: string,
  appliedFrames: ReadonlyArray<{ frame_role_id: string; source_link_name: string | null }> | null,
): string | null {
  const tool = toolOnFlange(document, componentId);
  if (tool !== null) return `${tool} default TCP`;
  if (appliedFrames === null || !appliedFrames.some((frame) => frame.frame_role_id === 'base')) return null;
  const link = (role: string) => appliedFrames.find((frame) => frame.frame_role_id === role)?.source_link_name ?? null;
  const tcp = link('tcp');
  if (tcp === null) return 'flange';
  return tcp === link('flange') ? `flange (${tcp})` : `robot TCP (${tcp})`;
}

/** The Draft's component rows (intent v2 documents only). */
export function draftComponents(document: Record<string, JsonValue> | null): Array<Record<string, JsonValue>> {
  const rows = document?.components;
  return Array.isArray(rows) ? rows.flatMap((row) => {
    const value = record(row);
    return value !== null && typeof value.component_id === 'string' ? [value] : [];
  }) : [];
}

/**
 * Draft components against Manager Applied component ids (none applied:
 * null). Without a Scenario (intent) Draft, the Applied components are listed
 * as they are.
 */
export function componentManagerRows(
  document: Record<string, JsonValue> | null,
  appliedIds: readonly string[] | null,
  profiles: Readonly<Record<string, ComponentProfileFacts>> = {},
): ComponentManagerRow[] {
  const applied = new Set(appliedIds ?? []);
  if (document?.api_version !== 'manyforge.deployment_intent.v2') {
    return [...applied].sort().map((id): ComponentManagerRow => ({
      componentId: id, profileId: null, kind: null, mount: null, change: 'applied',
    }));
  }
  const draft = draftComponents(document);
  const draftIds = new Set(draft.map((row) => row.component_id as string));
  const mounts = new Map(draftAttachments(document).map((row) => [
    row.child.componentId, `${row.parent.componentId}.${row.parent.interface}`,
  ]));
  return [
    ...draft.map((row): ComponentManagerRow => {
      const profileId = typeof row.component_profile_id === 'string' ? row.component_profile_id : null;
      const mount = mounts.get(row.component_id as string) ?? null;
      return {
        componentId: row.component_id as string,
        profileId,
        kind: mount !== null ? 'tool' : componentKindOf(row, profileId === null ? undefined : profiles[profileId]),
        mount,
        change: applied.has(row.component_id as string) ? 'applied' : 'added',
      };
    }),
    ...[...applied].filter((id) => !draftIds.has(id)).map((id): ComponentManagerRow => ({
      componentId: id, profileId: null, kind: null, mount: null, change: 'removed',
    })),
  ].sort((left, right) => left.componentId.localeCompare(right.componentId));
}

/** `id`, or the first `id_N` not in `taken`. */
export function nextFreeComponentId(id: string, taken: ReadonlySet<string>): string {
  if (!taken.has(id)) return id;
  for (let index = 2; ; index += 1) {
    if (!taken.has(`${id}_${index}`)) return `${id}_${index}`;
  }
}

/** A frame prefix derived from a component id (letters, digits, `_`, ending in `/`). */
export function framePrefixFor(componentId: string): string {
  return `${componentId.replace(/[^A-Za-z0-9_]/g, '_')}/`;
}

/**
 * The rows with components renamed: every `component_id` and
 * `controllable_component_id` naming a renamed component, and every
 * `source_id` it prefixes (`<id>.<signal>`).
 */
export function renameComponentRows(
  rows: ReadonlyArray<Record<string, JsonValue>>,
  renames: Readonly<Record<string, string>>,
): Array<Record<string, JsonValue>> {
  const rename = (value: JsonValue, key: string | null): JsonValue => {
    if (Array.isArray(value)) return value.map((item) => rename(item, null));
    const object = record(value);
    if (object !== null) {
      return Object.fromEntries(Object.entries(object).map(([field, item]) => [field, rename(item, field)]));
    }
    if (typeof value !== 'string') return value;
    if ((key === 'component_id' || key === 'controllable_component_id') && renames[value] !== undefined) {
      return renames[value];
    }
    if (key === 'source_id') {
      const [owner, ...rest] = value.split('.');
      if (rest.length > 0 && renames[owner] !== undefined) return [renames[owner], ...rest].join('.');
    }
    return value;
  };
  return rows.map((row) => rename(row, null) as Record<string, JsonValue>);
}

/**
 * The rows with every `robot_component_id` naming `robotId`: an added tool's
 * I/O companions (e.g. `xarm_digital` on the robot controller) talk to the
 * robot the tool is mounted on.
 */
export function withMountRobot(
  rows: ReadonlyArray<Record<string, JsonValue>>,
  robotId: string,
): Array<Record<string, JsonValue>> {
  const bind = (value: JsonValue, key: string | null): JsonValue => {
    if (Array.isArray(value)) return value.map((item) => bind(item, null));
    const object = record(value);
    if (object !== null) {
      return Object.fromEntries(Object.entries(object).map(([field, item]) => [field, bind(item, field)]));
    }
    return key === 'robot_component_id' && typeof value === 'string' ? robotId : value;
  };
  return rows.map((row) => bind(row, null) as Record<string, JsonValue>);
}

/** Why the renamed rows cannot be added to the Draft, or null. */
export function addComponentProblem(
  rows: ReadonlyArray<Record<string, JsonValue>>,
  document: Record<string, JsonValue> | null,
): string | null {
  const existing = new Set(draftComponents(document).map((row) => row.component_id as string));
  const seen = new Set<string>();
  for (const row of rows) {
    const id = row.component_id;
    if (typeof id !== 'string' || !COMPONENT_ID.test(id)) {
      return `Component id "${String(id)}" must start with a lowercase letter and use only a-z, 0-9, ".", "_" or "-".`;
    }
    if (existing.has(id)) return `The Deployment already has a component "${id}"; choose another id.`;
    if (seen.has(id)) return `Two added components use the id "${id}".`;
    seen.add(id);
  }
  return null;
}

/**
 * Each Program component role against the Draft: the Scenario's role
 * assignment names the component; the role is satisfied when the Draft has it
 * with the required type tags. The tags come from the catalog's
 * `componentProfiles`; a required tag on a profile without known tags is `unknown`.
 */
export function roleFeedback(
  componentRoles: Record<string, JsonValue> | null,
  assignment: Record<string, JsonValue> | null,
  rows: readonly ComponentManagerRow[],
  profiles: Readonly<Record<string, ComponentProfileFacts>>,
): RoleFeedback[] {
  const assigned = new Map<string, string>();
  const assignedRows = assignment?.component_roles;
  for (const row of Array.isArray(assignedRows) ? assignedRows : []) {
    const value = record(row);
    if (typeof value?.component_role_id === 'string' && typeof value.controllable_component_id === 'string') {
      assigned.set(value.component_role_id, value.controllable_component_id);
    }
  }
  return Object.entries(componentRoles ?? {}).map(([roleId, role]): RoleFeedback => {
    const requiredTags = strings(record(role)?.required_component_type_tags);
    const componentId = assigned.get(roleId) ?? null;
    const base = { roleId, componentId, requiredTags, pendingApply: false };
    if (componentId === null) {
      return { ...base, status: 'unbound', detail: 'The Scenario assigns no component to this role.' };
    }
    const row = rows.find((candidate) => candidate.componentId === componentId && candidate.change !== 'removed');
    if (row === undefined) {
      return { ...base, status: 'unbound',
        detail: `"${componentId}" is not in the Deployment Draft. Add a component with this id.` };
    }
    const tags = row.profileId === null ? undefined : profiles[row.profileId]?.tags;
    if (tags === undefined && requiredTags.length > 0) {
      return { ...base, status: 'unknown', pendingApply: row.change === 'added',
        detail: `The type of "${componentId}" is not known, so its fit for this role is not checked.` };
    }
    const missing = requiredTags.filter((tag) => !tags?.includes(tag));
    if (missing.length > 0) {
      return { ...base, status: 'incompatible',
        detail: `"${componentId}" lacks the required type ${missing.join(', ')}.` };
    }
    return { ...base, status: 'satisfied', pendingApply: row.change === 'added',
      detail: row.change === 'added' ? `"${componentId}" runs after the next Apply.` : `Bound to "${componentId}".` };
  });
}

/** Unbound roles whose required type tags the entry has (roles with requirements only). */
export function rolesFittingEntry(entry: ComponentCatalogEntry, feedback: readonly RoleFeedback[]): RoleFeedback[] {
  return feedback.filter((role) => role.status === 'unbound' && role.requiredTags.length > 0
    && role.requiredTags.every((tag) => entry.tags.includes(tag)));
}

export interface PreviewRecordChange {
  recordKind: string;
  added: string[];
  changed: string[];
  removed: string[];
}

/** What a Manager preview changes: component dispositions and record diffs. */
export function previewChanges(preview: ManagerDeploymentCandidatePreviewResponse): {
  components: ManagerDeploymentImpactComponent[];
  records: PreviewRecordChange[];
} {
  return {
    components: preview.impact.components.filter((row) => row.disposition !== 'retain'),
    records: Object.entries(preview.diff)
      .filter(([, diff]) => diff.added.length + diff.changed.length + diff.removed.length > 0)
      .map(([recordKind, diff]) => ({ recordKind, ...diff }))
      .sort((left, right) => left.recordKind.localeCompare(right.recordKind)),
  };
}

/** One Applied target of a component with its frames (source model). */
export interface ComponentFrameTarget {
  targetId: string;
  frames: Array<{ frameRoleId: string; sourceLinkName: string | null }>;
  /** What the `tcp` frame role resolves to (D1); null when not a motion target. */
  resolvedTcp: string | null;
}

/** The Applied frames of each of a component's targets, with the D1 `tcp` resolution. */
export function componentFrameTargets(
  targets: readonly SourceModelTarget[] | null,
  document: Record<string, JsonValue> | null,
  componentId: string,
): ComponentFrameTarget[] {
  return (targets ?? []).filter((target) => target.component_id === componentId).map((target) => {
    const frames = target.frames.map((frame) => ({
      frameRoleId: frame.frame_role_id, sourceLinkName: frame.source_link_name,
    }));
    return {
      targetId: target.target_id,
      frames,
      resolvedTcp: resolvedRobotTcp(document, componentId, target.frames),
    };
  }).sort((left, right) => left.targetId.localeCompare(right.targetId));
}

/** The authored gripper rows of a Draft (intent components or compiled runtime units). */
function gripperRows(document: Record<string, JsonValue> | null): Array<Record<string, JsonValue>> {
  const holders = document?.api_version === 'manyforge.deployment_intent.v2'
    ? document.components : record(document?.graph)?.runtime_units;
  return (Array.isArray(holders) ? holders : []).flatMap((holder) => {
    const configuration = record(record(holder)?.configuration);
    const runtime = record(configuration?.runtime_configuration) ?? configuration;
    const rows = runtime?.gripper_target_configurations;
    return Array.isArray(rows) ? rows.flatMap((row) => {
      const value = record(row);
      return value === null ? [] : [value];
    }) : [];
  });
}

/** A gripper target's authored frames in the Draft (its TCP, attach and mount frames). */
export interface DraftGripperFrames {
  targetId: string;
  tcpFrame: string;
  attachFrame: string;
  mountFrame: string;
}

export function draftGripperFrames(
  document: Record<string, JsonValue> | null,
  componentId: string,
): DraftGripperFrames[] {
  return gripperRows(document).flatMap((row) => (
    row.controllable_component_id === componentId && typeof row.command_target_id === 'string' ? [{
      targetId: row.command_target_id,
      tcpFrame: typeof row.tcp_frame === 'string' ? row.tcp_frame : '',
      attachFrame: typeof row.attach_frame === 'string' ? row.attach_frame : '',
      mountFrame: typeof row.mount_frame === 'string' ? row.mount_frame : '',
    }] : []));
}

/** One I/O signal as the component page lists it. */
export interface ComponentSignal {
  /** The component that declares the signal. */
  componentId: string;
  signalName: string;
  direction: 'input' | 'output';
  /** Where it comes from, or how the component uses it. */
  detail: string;
}

const PNEUMATIC_INPUT_LABELS: Record<string, string> = {
  open: 'Open input', closed: 'Closed input', pressure: 'Pressure input', workpiece_grasped: 'Workpiece grasped input',
};

/** The signals a gripper row's `pneumatic_io` names, with their use. */
function pneumaticReferences(row: Record<string, JsonValue>): ComponentSignal[] {
  const io = record(row.pneumatic_io);
  const signal = (value: JsonValue | undefined) => {
    const named = record(record(value)?.signal);
    return typeof named?.controllable_component_id === 'string' && typeof named.signal_name === 'string'
      ? { componentId: named.controllable_component_id, signalName: named.signal_name } : null;
  };
  const outputs = (Array.isArray(io?.outputs) ? io.outputs : []).flatMap((value) => {
    const found = signal(value);
    return found === null ? [] : [{ ...found, direction: 'output' as const, detail: 'valve output' }];
  });
  const inputs = Object.entries(record(io?.inputs) ?? {}).flatMap(([key, value]) => {
    const found = signal(value);
    return found === null ? [] : [{
      ...found, direction: 'input' as const, detail: PNEUMATIC_INPUT_LABELS[key] ?? `${key} input`,
    }];
  });
  return [...outputs, ...inputs];
}

/**
 * The I/O signals around one component in the Draft: those it declares
 * (`owned`), other components' signals it uses (`uses`: a pneumatic gripper's
 * valves and sensors), and other components' signals that use it (`usedBy`:
 * its own signals in another gripper's pneumatic I/O, an xArm pin on this
 * robot, a geometric-contact input that follows the robot's Active TCP).
 */
export function componentSignals(
  document: Record<string, JsonValue> | null,
  componentId: string,
): { owned: ComponentSignal[]; uses: ComponentSignal[]; usedBy: ComponentSignal[] } {
  const inputs = declaredIoInputs(document);
  const outputs = declaredIoOutputs(document);
  const owned = [
    ...inputs.filter((draft) => draft.componentId === componentId).map((draft): ComponentSignal => ({
      componentId, signalName: draft.signalName, direction: 'input', detail: ioInputSourceText(draft),
    })),
    ...outputs.filter((draft) => draft.componentId === componentId).map((draft): ComponentSignal => ({
      componentId, signalName: draft.signalName, direction: 'output',
      detail: draft.profile === 'xarm_digital'
        ? `xArm ${draft.robotComponentId} ${draft.bank} digital output pin ${draft.pin}`
        : IO_OUTPUT_PROFILE_LABELS[draft.profile],
    })),
  ];
  const rows = gripperRows(document);
  const uses = rows.filter((row) => row.controllable_component_id === componentId)
    .flatMap((row) => pneumaticReferences(row)
      .filter((reference) => reference.componentId !== componentId)
      .map((reference): ComponentSignal => ({
        ...reference, detail: `${reference.detail} of ${String(row.command_target_id)}`,
      })));
  const usedBy = [
    ...rows.filter((row) => row.controllable_component_id !== componentId).flatMap((row) => (
      pneumaticReferences(row).filter((reference) => reference.componentId === componentId)
        .map((reference): ComponentSignal => ({
          ...reference, detail: `${reference.detail} of ${String(row.controllable_component_id)}`,
        })))),
    ...[...inputs, ...outputs].filter((draft) => draft.componentId !== componentId
      && draft.profile === 'xarm_digital' && draft.robotComponentId === componentId)
      .map((draft): ComponentSignal => ({
        componentId: draft.componentId, signalName: draft.signalName,
        direction: 'echoEnabled' in draft ? 'input' : 'output',
        detail: `${draft.bank} digital pin ${draft.pin} on this robot`,
      })),
    ...inputs.filter((draft) => draft.componentId !== componentId
      && draft.profile === 'geometric_contact' && draft.robotComponentId === componentId)
      .map((draft): ComponentSignal => ({
        componentId: draft.componentId, signalName: draft.signalName, direction: 'input',
        detail: `Active TCP contact with ${draft.targetSceneResourceId}`,
      })),
  ];
  const order = (left: ComponentSignal, right: ComponentSignal) => (
    left.componentId.localeCompare(right.componentId) || left.signalName.localeCompare(right.signalName));
  return { owned: owned.sort(order), uses: uses.sort(order), usedBy: usedBy.sort(order) };
}

/** The Program roles the Scenario assigns to a component, as their feedback rows. */
export function rolesOfComponent(feedback: readonly RoleFeedback[], componentId: string): RoleFeedback[] {
  return feedback.filter((role) => role.componentId === componentId);
}
