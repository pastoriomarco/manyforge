import type {
  JsonValue,
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
  MachineCatalogEntry,
} from '../types/componentCatalog';
import { draftAttachments, fromDraft } from '../state/kinematicIndex';
import { componentTree, flattenComponentTree } from '../state/componentPresentation';

/**
 * Pure helpers of the Components section (one page per component) and of the
 * Deployment tabs (Overview, Program binding, Cell settings, Changes). The Deployment
 * Draft, Manager Applied and the Manager preview stay the authorities; these
 * only present them and prepare an Add component edit.
 */

export type ComponentManagerTab = 'overview' | 'binding' | 'cell' | 'changes';

/** One component of the Draft or of Manager Applied. */
export interface ComponentManagerRow {
  componentId: string;
  profileId: string | null;
  /** Null when only Manager Applied names the component. */
  kind: ComponentCatalogKind | null;
  /** A mounted tool's parent interface, `<robot>.<interface>`; null otherwise. */
  mount: string | null;
  /** A member's owner component; null for owners and owner-less components. */
  owner: string | null;
  /** A member's slot in its machine (`arm`, `controller_io`, ...); null otherwise. */
  role: string | null;
  /** False for a deactivated member (`active: false`): it stays in the Draft, the compiler leaves it out. */
  active: boolean;
  /** `applied`: in both; `added`: Draft only (runs after Apply); `removed`: Applied only. */
  change: 'applied' | 'added' | 'removed';
  /** Its level in the list's tree: 0 at the top, +1 under its owner or the component it is mounted on. */
  depth: number;
}

export const COMPONENT_KIND_LABELS: Record<ComponentCatalogKind, string> = {
  machine: 'Machine',
  robot: 'Robot',
  tool: 'Tool',
  io_device: 'I/O device',
  pose_source: 'Pose source',
  ai_model: 'AI model',
  sensor: 'Sensor',
  assistant: 'AI assistant',
};

/** The kind a row's `runtime_kind` names (other rows carry a robot or tool model). */
const RUNTIME_KINDS: Record<string, ComponentCatalogKind> = {
  io_provider: 'io_device',
  pose_source: 'pose_source',
  ai_model: 'ai_model',
  sensor: 'sensor',
  assistant: 'assistant',
};

/** The Deployment intent's component id rule (`deployment_intent.py`). */
const COMPONENT_ID = /^[a-z][a-z0-9._-]{0,127}$/;

function record(value: JsonValue | undefined): Record<string, JsonValue> | null {
  return value !== null && value !== undefined && typeof value === 'object' && !Array.isArray(value)
    ? value : null;
}

/**
 * A model row is a tool when its component type is tagged `tool`, else a
 * robot; a member without a model source is an I/O bank (its signals come
 * from its model).
 */
export function componentKindOf(
  row: Record<string, JsonValue>,
  facts?: ComponentProfileFacts,
): ComponentCatalogKind {
  const runtimeKind = record(row.configuration)?.runtime_kind;
  return (typeof runtimeKind === 'string' ? RUNTIME_KINDS[runtimeKind] : undefined)
    ?? (typeof row.owner === 'string' && row.model_source === undefined ? 'io_device' : undefined)
    ?? (facts?.tags.includes('tool') ? 'tool' : 'robot');
}

/** The owner a Draft row names, or null. */
export function ownerOf(row: Record<string, JsonValue>): string | null {
  return typeof row.owner === 'string' ? row.owner : null;
}

/** The Draft rows of the members of `ownerId`, in Draft order. */
export function machineMembers(
  document: Record<string, JsonValue> | null,
  ownerId: string,
): Array<Record<string, JsonValue>> {
  return draftComponents(document).filter((row) => ownerOf(row) === ownerId);
}

/**
 * The Draft's `components` and `attachments` without a machine: the owner,
 * its members and every attachment naming one of them (a tool mounted on the
 * arm stays in the Draft, unmounted).
 */
export function withoutMachine(
  document: Record<string, JsonValue>,
  ownerId: string,
): { components: JsonValue[]; attachments: JsonValue[] } {
  const removed = new Set([ownerId, ...machineMembers(document, ownerId).map((row) => row.component_id as string)]);
  const components = Array.isArray(document.components) ? document.components : [];
  const attachments = Array.isArray(document.attachments) ? document.attachments : [];
  const named = (value: JsonValue, side: 'parent' | 'child') => {
    const id = record(record(value)?.[side])?.component_id;
    return typeof id === 'string' && removed.has(id);
  };
  return {
    components: components.filter((value) => !removed.has(String(record(value)?.component_id))),
    attachments: attachments.filter((value) => !named(value, 'parent') && !named(value, 'child')),
  };
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
      componentId: id, profileId: null, kind: null, mount: null, owner: null, role: null, active: true, change: 'applied',
      depth: 0,
    }));
  }
  const draft = draftComponents(document);
  const draftIds = new Set(draft.map((row) => row.component_id as string));
  const owners = new Set(draft.flatMap((row) => ownerOf(row) ?? []));
  const mounts = new Map(draftAttachments(document).map((row) => [
    row.child.componentId, `${row.parent.componentId}.${row.parent.interface}`,
  ]));
  const parents = mountParents(document);
  return componentListTree([
    ...draft.map((row): ComponentManagerRow => {
      const id = row.component_id as string;
      const profileId = typeof row.component_profile_id === 'string' ? row.component_profile_id : null;
      const mount = mounts.get(id) ?? null;
      return {
        componentId: id,
        profileId,
        kind: mount !== null ? 'tool' : owners.has(id) ? 'machine'
          : componentKindOf(row, profileId === null ? undefined : profiles[profileId]),
        mount,
        owner: ownerOf(row),
        role: typeof row.role === 'string' ? row.role : null,
        active: row.active !== false,
        change: applied.has(id) ? 'applied' : 'added',
        depth: 0,
      };
    }),
    ...[...applied].filter((id) => !draftIds.has(id)).map((id): ComponentManagerRow => ({
      componentId: id, profileId: null, kind: null, mount: null, owner: null, role: null, active: true, change: 'removed',
      depth: 0,
    })),
  ].sort((left, right) => left.componentId.localeCompare(right.componentId)), parents);
}

/** Sorted rows as one tree: members under their owner, a mounted component under its parent. */
function componentListTree(
  rows: ComponentManagerRow[],
  mounts: Readonly<Record<string, string>>,
): ComponentManagerRow[] {
  return flattenComponentTree(componentTree(rows, (row) => row.componentId,
    (row) => row.owner ?? mounts[row.componentId] ?? null))
    .map(({ item, depth }) => ({ ...item, depth }));
}

/** Each mounted component's parent component in the Draft's `attachments[]` (child id to parent id). */
export function mountParents(document: Record<string, JsonValue> | null): Record<string, string> {
  return Object.fromEntries(draftAttachments(document).map((row) => [row.child.componentId, row.parent.componentId]));
}

/** A member slot's own label, in place of its kind; other slots show after the kind. */
const ROLE_LABELS: Readonly<Record<string, string>> = { arm: 'Arm' };

/**
 * A list row's secondary text: its kind (a mounted tool with its interface,
 * the whole mount when its parent isn't the row above it; a member by its
 * slot), then its model. An I/O member shows its slot instead of its model.
 */
export function componentRowText(row: ComponentManagerRow, underParent: boolean): string {
  const roleLabel = row.role === null ? undefined : ROLE_LABELS[row.role];
  const kind = row.mount !== null ? `Tool on ${underParent ? row.mount.slice(row.mount.lastIndexOf('.') + 1) : row.mount}`
    : roleLabel ?? (row.kind === null ? 'Component' : COMPONENT_KIND_LABELS[row.kind]);
  const detail = row.role !== null && roleLabel === undefined ? row.role : row.profileId;
  return detail === null ? kind : `${kind} · ${detail}`;
}

/** `id`, or the first `id_N` not in `taken`. */
export function nextFreeComponentId(id: string, taken: ReadonlySet<string>): string {
  if (!taken.has(id)) return id;
  for (let index = 2; ; index += 1) {
    if (!taken.has(`${id}_${index}`)) return `${id}_${index}`;
  }
}

/** A member's default id. */
export function defaultMemberId(ownerId: string, role: string): string {
  return `${ownerId}_${role}`;
}

/**
 * Add component for a machine model: one entry with the model's ready
 * rows as they are (ids
 * `default_owner_id` and `<default_owner_id>_<role>`, inner references
 * included): the owner on `lane`, each member naming its owner and role.
 * Id edits rename them like any catalog entry (`renameComponentRows`).
 */
export function machineCatalogEntry(
  machine: MachineCatalogEntry,
  lane: string,
  profiles: Readonly<Record<string, ComponentProfileFacts>>,
): ComponentCatalogEntry {
  const ownerId = machine.defaultOwnerId;
  const members = machine.members.map((member): Record<string, JsonValue> => ({
    ...member.row, owner: ownerId, role: member.role,
  }));
  const armProfile = members.find((row) => row.model_source !== undefined)?.component_profile_id;
  return {
    entryId: `machine/${machine.modelId}`,
    kind: 'machine',
    componentId: ownerId,
    componentProfileId: machine.modelId,
    modelFamilyId: null,
    tags: typeof armProfile === 'string' ? profiles[armProfile]?.tags ?? [] : [],
    lane,
    description: machine.title,
    sources: [],
    components: [{ ...machine.ownerRow, lane }, ...members],
  };
}

/** A frame prefix derived from a component id (letters, digits, `_`, ending in `/`). */
export function framePrefixFor(componentId: string): string {
  return `${componentId.replace(/[^A-Za-z0-9_]/g, '_')}/`;
}

/**
 * The rows with components renamed: every `component_id` and
 * `controllable_component_id` naming a renamed component, a member row's
 * `owner`, and every `source_id` it prefixes (`<id>.<signal>`).
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
  return rows.map((row) => {
    const renamed = rename(row, null) as Record<string, JsonValue>;
    const owner = ownerOf(row);
    return owner !== null && renames[owner] !== undefined ? { ...renamed, owner: renames[owner] } : renamed;
  });
}

/**
 * The rows with every `robot_component_id` naming `robotId`: an added tool's
 * rows that follow the robot the tool is mounted on (e.g. a geometric contact
 * input). A member's `owner` is not a robot reference and stays.
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
 * its own signals in another gripper's pneumatic I/O, a geometric-contact
 * input that follows the robot's Active TCP).
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
      detail: IO_OUTPUT_PROFILE_LABELS[draft.profile],
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
