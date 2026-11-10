import { useMemo, useState } from 'react';

import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { JsonValue } from '../types/api';
import type {
  ComponentCatalog,
  ComponentCatalogEntry,
  ComponentCatalogKind,
  MachineCatalogEntry,
} from '../types/componentCatalog';
import {
  COMPONENT_KIND_LABELS,
  addComponentProblem,
  defaultMemberId,
  draftComponents,
  framePrefixFor,
  machineCatalogEntry,
  mountCandidates,
  nextFreeComponentId,
  renameComponentRows,
  toolMountRow,
  withMountRobot,
} from '../utils/componentManager';
import { aiModelComponents } from './AiModelComponentForm';
import { DEFAULT_ASSISTANT_PROFILE_ID, NO_AI_MODEL_FOR_ASSISTANT } from './AssistantComponentForm';

const KINDS: readonly ComponentCatalogKind[] = ['machine', 'robot', 'tool', 'io_device', 'pose_source', 'ai_model', 'sensor', 'assistant'];

/** An absolute ROS topic name (the compiler checks it fully). */
const ROS_TOPIC = /^\/[A-Za-z0-9_/~{}]*$/;

interface AddDraft {
  entry: ComponentCatalogEntry;
  /** A machine model: `entry` is its owner and member rows. */
  machine: MachineCatalogEntry | null;
  /** A machine's lane (its owner's; the members take it). */
  lane: string;
  /** A sensor's source topic (`runtime_configuration.topic`); empty for other kinds. */
  topic: string;
  /** A sensor's source frame (`runtime_configuration.source_frame_id`); empty for other kinds. */
  sourceFrame: string;
  /** Catalog component id -> id in this Deployment. */
  ids: Record<string, string>;
  framePrefix: string;
  position: [string, string, string];
  /** A tool's robot (its `flange`); empty for other kinds. */
  mountOn: string;
}

/** The row the role, prefix and base position apply to: a machine's arm (the member with a model), else the entry's own. */
function mainRowId(entry: ComponentCatalogEntry): string {
  if (entry.kind !== 'machine') return entry.componentId;
  const arm = entry.components.find((row) => row.owner !== undefined && row.model_source !== undefined);
  return typeof arm?.component_id === 'string' ? arm.component_id : entry.componentId;
}

/** Whether the entry places a model in the cell (base position and frame prefix). */
function placed(entry: ComponentCatalogEntry): boolean {
  return entry.kind === 'robot' || (entry.kind === 'machine' && mainRowId(entry) !== entry.componentId);
}

function object(value: JsonValue | undefined): Record<string, JsonValue> | null {
  return value !== null && value !== undefined && typeof value === 'object' && !Array.isArray(value)
    ? value : null;
}

function rowPosition(row: Record<string, JsonValue> | undefined): [string, string, string] {
  const position = object(row?.root_pose_in_cell)?.position_m;
  return Array.isArray(position) && position.length === 3
    ? position.map((value) => String(value)) as [string, string, string]
    : ['0', '0', '0'];
}

/** A first choice for the ids: a fitting unbound role's assigned id, else the catalog id (made free). */
function initialDraft(
  entry: ComponentCatalogEntry,
  machine: MachineCatalogEntry | null,
  document: Record<string, JsonValue> | null,
  mounts: readonly string[],
): AddDraft {
  const existing = draftComponents(document);
  const taken = new Set(existing.map((row) => row.component_id as string));
  const ids: Record<string, string> = {};
  const mainId = mainRowId(entry);
  for (const row of entry.components) {
    const original = row.component_id as string;
    // A machine's members follow a free owner id: `<owner_id>_<role>`.
    const memberDefault = machine !== null && typeof row.role === 'string' && ids[entry.componentId] !== undefined
      ? defaultMemberId(ids[entry.componentId], row.role) : original;
    ids[original] = nextFreeComponentId(memberDefault, taken);
    taken.add(ids[original]);
  }
  const main = entry.components.find((row) => row.component_id === mainId);
  // The same model twice needs a frame prefix to keep its frame names apart.
  const urdf = JSON.stringify(object(main?.model_source)?.urdf ?? null);
  const sameModel = main?.model_source !== undefined && existing.some((row) => (
    JSON.stringify(object(row.model_source)?.urdf ?? null) === urdf
  ));
  return {
    entry,
    machine,
    lane: entry.lane ?? '',
    topic: '',
    sourceFrame: '',
    ids,
    framePrefix: sameModel ? framePrefixFor(ids[mainId]) : String(main?.frame_prefix ?? ''),
    position: rowPosition(main),
    mountOn: entry.kind === 'tool' ? mounts[0] ?? '' : '',
  };
}

/** Rename one catalog id; renaming a machine's owner moves the member ids still at their default. */
function withId(draft: AddDraft, original: string, id: string): AddDraft {
  const ids = { ...draft.ids, [original]: id };
  if (draft.machine !== null && original === draft.entry.componentId) {
    for (const row of draft.entry.components) {
      if (typeof row.role !== 'string') continue;
      const member = row.component_id as string;
      if (draft.ids[member] === defaultMemberId(draft.ids[original], row.role)) {
        ids[member] = defaultMemberId(id, row.role);
      }
    }
  }
  return { ...draft, ids };
}

/**
 * The Draft rows the add sends: renamed, with the main robot's prefix and
 * position. A tool gets its prefix and no `root_pose_in_cell`: its mount row
 * places it on the robot flange. A machine's rows are built for its lane;
 * the members' `owner` follows the owner's id.
 */
function addedComponentRows(
  draft: AddDraft,
  catalog: ComponentCatalog | null,
  document: Record<string, JsonValue> | null,
): Array<Record<string, JsonValue>> | string {
  const entry = draft.machine === null ? draft.entry
    : machineCatalogEntry(draft.machine, draft.lane, catalog?.componentProfiles ?? {});
  const mainId = draft.ids[mainRowId(entry)];
  const position = draft.position.map((value) => Number(value));
  if (placed(entry) && draft.position.some((value, index) => (
    value.trim() === '' || !Number.isFinite(position[index])
  ))) {
    return 'Position must be three finite numbers in meters.';
  }
  if (draft.framePrefix !== '' && !/^[A-Za-z0-9_][A-Za-z0-9_/]*\/$/.test(draft.framePrefix)) {
    return "Frame prefix must be empty or use only ASCII letters, digits, '_' and '/', ending in '/'.";
  }
  if (entry.kind === 'sensor' && !ROS_TOPIC.test(draft.topic.trim())) {
    return 'Topic must be an absolute ROS topic, e.g. /camera/points.';
  }
  if (entry.kind === 'sensor' && draft.sourceFrame.trim() === '') {
    return 'Enter the frame the cloud is published in.';
  }
  const aiModels = aiModelComponents(document);
  if (entry.kind === 'assistant' && aiModels.length === 0) return NO_AI_MODEL_FOR_ASSISTANT;
  const renamed = renameComponentRows(entry.components, draft.ids);
  const rows = entry.kind === 'tool' ? withMountRobot(renamed, draft.mountOn) : renamed;
  return rows.map((row) => {
    if (row.component_id !== mainId) return row;
    if (entry.kind === 'sensor') {
      // A sensor has no lane; its source topic and frame are the settings without a default.
      const configuration = object(row.configuration) ?? {};
      return {
        ...row,
        configuration: {
          ...configuration,
          runtime_configuration: {
            ...(object(configuration.runtime_configuration) ?? {}),
            topic: draft.topic.trim(),
            source_frame_id: draft.sourceFrame.trim(),
          },
        },
      };
    }
    if (entry.kind === 'assistant') {
      // Its AI model (the only one; several: chosen on its page) and the default chat profile.
      const configuration = object(row.configuration) ?? {};
      return {
        ...row,
        configuration: {
          ...configuration,
          runtime_configuration: {
            ...(object(configuration.runtime_configuration) ?? {}),
            ...(aiModels.length === 1 ? { model_component_id: aiModels[0].componentId } : {}),
            assistant_profile_id: DEFAULT_ASSISTANT_PROFILE_ID,
          },
        },
      };
    }
    if (entry.kind === 'tool') {
      const mounted = Object.fromEntries(Object.entries(row).filter(([key]) => key !== 'root_pose_in_cell'));
      return { ...mounted, frame_prefix: draft.framePrefix };
    }
    if (!placed(entry)) return row;
    const pose = object(row.root_pose_in_cell) ?? {};
    return {
      ...row,
      frame_prefix: draft.framePrefix,
      root_pose_in_cell: { ...pose, position_m: position },
    };
  });
}

/**
 * Add component: compose components that already exist (the curated catalog)
 * into the Deployment Draft, like every other Draft edit (remove, mount, base
 * pose, lane). Nothing runs until Apply.
 */
export function AddComponentPanel({ controller, catalog, catalogError, onAdded }: {
  controller: DeploymentWorkspaceController;
  catalog: ComponentCatalog | null;
  catalogError: string | null;
  /** Called with the added main component's id. */
  onAdded: (componentId: string) => void;
}) {
  const [open, setOpen] = useState(false);
  const [kind, setKind] = useState<ComponentCatalogKind>('robot');
  const [draft, setDraft] = useState<AddDraft | null>(null);
  // Local validation (red, blocks Add until an edit) or a pointer to the owner's refusal (neutral).
  const [error, setError] = useState<{ message: string; refused: boolean } | null>(null);
  const document = controller.workspace?.draft.document ?? null;
  // A machine model is offered as one entry: its owner and member rows.
  const entries = useMemo((): Array<{ entry: ComponentCatalogEntry; machine: MachineCatalogEntry | null }> => (
    kind === 'machine'
      ? (catalog?.machines ?? []).map((machine) => ({
        machine,
        entry: machineCatalogEntry(machine, machine.lanes[0] ?? '', catalog?.componentProfiles ?? {}),
      }))
      : (catalog?.entries ?? []).filter((entry) => entry.kind === kind).map((entry) => ({ entry, machine: null }))
  ), [catalog, kind]);
  const mounts = useMemo(
    () => mountCandidates(document, catalog?.componentProfiles ?? {}),
    [catalog, document],
  );
  const unavailable = document === null
    ? 'Open or create a Scenario first.'
    : document.api_version !== 'manyforge.deployment_intent.v2'
      ? 'Add component needs a Scenario Deployment (source-referenced intent).'
      : !controller.mutationAvailable
        ? controller.mutationUnavailableReason ?? 'Deployment Draft edits are unavailable.'
        : null;
  const disabled = unavailable !== null || controller.busy;

  const add = async () => {
    if (draft === null) return;
    const rows = addedComponentRows(draft, catalog, document);
    if (typeof rows === 'string') {
      setError({ message: rows, refused: false });
      return;
    }
    const problem = draft.entry.kind === 'tool' && !mounts.includes(draft.mountOn)
      ? 'Choose a robot with a free flange to mount this tool on.'
      : addComponentProblem(rows, document);
    if (problem !== null) {
      setError({ message: problem, refused: false });
      return;
    }
    // A tool is added together with the attachment row that mounts it.
    const attachments = draft.entry.kind === 'tool'
      ? [toolMountRow(draft.mountOn, draft.ids[draft.entry.componentId])] : [];
    const result = await controller.addComponents(rows, attachments);
    if (result === null) {
      setError({ message: 'The Deployment owner did not add this component; its reason is shown in the notices.', refused: true });
      return;
    }
    setDraft(null);
    setOpen(false);
    onAdded(draft.ids[draft.entry.componentId]);
  };

  if (!open) {
    return (
      <div className="dactions">
        <button type="button" className="primary-action" disabled={disabled}
          title={unavailable ?? 'Add a machine, robot, tool, I/O device, pose source, AI model, sensor or AI assistant to the Deployment Draft.'}
          onClick={() => { setOpen(true); setError(null); }}>Add component</button>
      </div>
    );
  }
  return (
    <section className="dcard" aria-labelledby="add-component-title">
      <header>
        <h4 id="add-component-title">Add component</h4>
        <button type="button" className="quiet-button" disabled={controller.busy}
          onClick={() => { setOpen(false); setDraft(null); setError(null); }}>Cancel</button>
      </header>
      <p className="field-help">
        Pick a component that already exists in a ManyForge example Scenario, a machine model (its
        owner and every member) or a sensor model. It is added to the Deployment Draft only; Preview and Apply bring it
        up prepared, then Enable it.
      </p>
      {/* Yellow: a failed read blocks nothing else. */}
      {catalogError !== null && <p className="inline-message severity-warning" role="alert">{catalogError}</p>}
      {catalog === null && catalogError === null && <p className="muted" role="status">Reading the component catalog…</p>}
      <div className="dseg" role="radiogroup" aria-label="Component kind">
        {KINDS.map((option) => (
          <button key={option} type="button" role="radio" aria-checked={kind === option}
            className={kind === option ? 'active' : undefined}
            onClick={() => { setKind(option); setDraft(null); setError(null); }}>
            {COMPONENT_KIND_LABELS[option]}
          </button>
        ))}
      </div>
      <div className="drows" role="listbox" aria-label="Available components">
        {catalog !== null && entries.length === 0 && <p className="muted">No component of this kind is available.</p>}
        {entries.map(({ entry, machine }) => {
          const selected = draft?.entry.entryId === entry.entryId;
          const others = entry.components.map((row) => row.component_id as string)
            .filter((id) => id !== entry.componentId);
          return (
            <button key={entry.entryId} type="button" role="option" aria-selected={selected}
              className={`dpick${selected ? ' selected' : ''}`}
              title={entry.description ?? undefined}
              onClick={() => {
                setDraft(initialDraft(entry, machine, document, mounts));
                setError(null);
              }}>
              <span>
                <strong>{machine?.title ?? entry.componentProfileId}</strong>
                <small>
                  {entry.componentId}
                  {others.length > 0 ? ` + ${others.join(', ')}` : ''}
                  {machine !== null ? ` · ${machine.modelId}` : entry.lane !== null ? ` · ${entry.lane}` : ''}
                  {entry.kind === 'sensor' || entry.kind === 'assistant' ? ` · ${entry.description ?? ''}` : ''}
                  {entry.tags.length > 0 ? ` · ${entry.tags.join(', ')}` : ''}
                </small>
              </span>
              <small>
                {machine !== null ? `${machine.members.length} members`
                    : entry.kind === 'sensor' ? 'sensor model'
                    : entry.kind === 'assistant' ? 'assistant model'
                    : `from ${entry.sources[0]}${entry.sources.length > 1 ? ` +${entry.sources.length - 1}` : ''}`}
              </small>
            </button>
          );
        })}
      </div>
      {catalog !== null && catalog.profilesWithoutEntry.length > 0 && (
        <p className="field-help">
          Also defined, without a ready component row yet: {catalog.profilesWithoutEntry.join(', ')}.
          Write its row in a Scenario first (see docs/authoring/ADDING_A_COMPONENT.md).
        </p>
      )}
      {draft !== null && (
        <fieldset className="dgrid" aria-label="New component settings">
          {draft.machine !== null && (
            <label>
              <span>Lane</span>
              <select aria-label="New machine lane" value={draft.lane} disabled={controller.busy}
                onChange={(event) => { setDraft({ ...draft, lane: event.target.value }); setError(null); }}>
                {draft.machine.lanes.map((lane) => <option key={lane} value={lane}>{lane}</option>)}
              </select>
              <small>The machine's lane; every member runs on it.</small>
            </label>
          )}
          {draft.entry.kind === 'sensor' && (
            <>
              <label>
                <span>Topic</span>
                <input aria-label="New sensor topic" value={draft.topic} disabled={controller.busy}
                  placeholder="/camera/points"
                  onChange={(event) => { setDraft({ ...draft, topic: event.target.value }); setError(null); }} />
                <small>The source topic it reads (absolute); it shows no data until something publishes it.</small>
              </label>
              <label>
                <span>Source frame</span>
                <input aria-label="New sensor source frame" value={draft.sourceFrame} disabled={controller.busy}
                  placeholder="camera_link"
                  onChange={(event) => { setDraft({ ...draft, sourceFrame: event.target.value }); setError(null); }} />
                <small>The frame the cloud is published in; its other settings start at their defaults.</small>
              </label>
            </>
          )}
          {draft.entry.components.map((row) => {
            const original = row.component_id as string;
            const id = draft.ids[original];
            const slot = draft.machine?.members.find((member) => member.role === row.role);
            const name = draft.machine === null ? original
              : slot === undefined ? 'machine (owner)' : `${slot.title}, ${slot.role}`;
            return (
              <label key={original}>
                <span>Component id{draft.entry.components.length > 1 ? ` (${name})` : ''}</span>
                <input aria-label={`Component id for ${original}`} value={id} disabled={controller.busy}
                  onChange={(event) => {
                    setDraft(withId(draft, original, event.target.value));
                    setError(null);
                  }} />
              </label>
            );
          })}
          {draft.entry.kind === 'tool' && (
            <label>
              <span>Mount on</span>
              <select aria-label="New tool mount" value={draft.mountOn} disabled={controller.busy}
                onChange={(event) => { setDraft({ ...draft, mountOn: event.target.value }); setError(null); }}>
                {mounts.length === 0 && <option value="">No robot with a free flange</option>}
                {mounts.map((id) => <option key={id} value={id}>{id}.flange</option>)}
              </select>
              <small>{mounts.length === 0
                ? 'Add a robot first: each robot flange takes one tool.'
                : 'The tool is mounted on this robot\'s flange; its default TCP becomes the robot\'s tcp.'}</small>
            </label>
          )}
          {(placed(draft.entry) || draft.entry.kind === 'tool') && (
            <label>
              <span>TF frame prefix</span>
              <input aria-label="New component frame prefix" value={draft.framePrefix} disabled={controller.busy}
                onChange={(event) => { setDraft({ ...draft, framePrefix: event.target.value }); setError(null); }} />
              <small>Needed when the same model is already in the cell; empty keeps unprefixed frames.</small>
            </label>
          )}
          {placed(draft.entry) && (
            <>
              {(['x', 'y', 'z'] as const).map((axis, index) => (
                <label key={axis}>
                  <span>Base {axis} (m)</span>
                  <input aria-label={`New component base ${axis}`} inputMode="decimal"
                    value={draft.position[index]} disabled={controller.busy}
                    onChange={(event) => {
                      const position = [...draft.position] as AddDraft['position'];
                      position[index] = event.target.value;
                      setDraft({ ...draft, position });
                      setError(null);
                    }} />
                </label>
              ))}
            </>
          )}
        </fieldset>
      )}
      {error !== null && (error.refused
        ? <p className="inline-message severity-info" role="status">{error.message}</p>
        : <p className="inline-message severity-error" role="alert">{error.message}</p>)}
      <div className="dactions">
        <button type="button" className="primary-action" disabled={disabled || draft === null}
          title={unavailable ?? (draft === null ? 'Pick a component first.' : 'Add to the Deployment Draft; nothing runs until Apply.')}
          onClick={() => { void add(); }}>Add to Draft</button>
      </div>
    </section>
  );
}
