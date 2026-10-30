import { useMemo, useState } from 'react';

import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { JsonValue } from '../types/api';
import type {
  ComponentCatalog,
  ComponentCatalogEntry,
  ComponentCatalogKind,
} from '../types/componentCatalog';
import {
  COMPONENT_KIND_LABELS,
  addComponentProblem,
  draftComponents,
  framePrefixFor,
  mountCandidates,
  nextFreeComponentId,
  renameComponentRows,
  rolesFittingEntry,
  toolMountRow,
  withMountRobot,
  type RoleFeedback,
} from '../utils/componentManager';

const KINDS: readonly ComponentCatalogKind[] = ['robot', 'tool', 'io_device', 'pose_source', 'ai_model'];

interface AddDraft {
  entry: ComponentCatalogEntry;
  /** Catalog component id -> id in this Deployment. */
  ids: Record<string, string>;
  framePrefix: string;
  position: [string, string, string];
  /** A tool's robot (its `flange`); empty for other kinds. */
  mountOn: string;
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
  document: Record<string, JsonValue> | null,
  feedback: readonly RoleFeedback[],
  mounts: readonly string[],
): AddDraft {
  const existing = draftComponents(document);
  const taken = new Set(existing.map((row) => row.component_id as string));
  const ids: Record<string, string> = {};
  const roleId = rolesFittingEntry(entry, feedback)
    .map((role) => role.componentId)
    .find((id): id is string => id !== null && !taken.has(id));
  for (const row of entry.components) {
    const original = row.component_id as string;
    ids[original] = original === entry.componentId && roleId !== undefined
      ? roleId : nextFreeComponentId(original, taken);
    taken.add(ids[original]);
  }
  const main = entry.components.find((row) => row.component_id === entry.componentId);
  // The same model twice needs a frame prefix to keep its frame names apart.
  const urdf = JSON.stringify(object(main?.model_source)?.urdf ?? null);
  const sameModel = main?.model_source !== undefined && existing.some((row) => (
    JSON.stringify(object(row.model_source)?.urdf ?? null) === urdf
  ));
  return {
    entry,
    ids,
    framePrefix: sameModel ? framePrefixFor(ids[entry.componentId]) : String(main?.frame_prefix ?? ''),
    position: rowPosition(main),
    mountOn: entry.kind === 'tool' ? mounts[0] ?? '' : '',
  };
}

/**
 * The Draft rows the add sends: renamed, with the main robot's prefix and
 * position. A tool gets its prefix and no `root_pose_in_cell`: its mount row
 * places it on the robot flange.
 */
function addedComponentRows(draft: AddDraft): Array<Record<string, JsonValue>> | string {
  const mainId = draft.ids[draft.entry.componentId];
  const position = draft.position.map((value) => Number(value));
  if (draft.entry.kind === 'robot' && draft.position.some((value, index) => (
    value.trim() === '' || !Number.isFinite(position[index])
  ))) {
    return 'Position must be three finite numbers in meters.';
  }
  if (draft.framePrefix !== '' && !/^[A-Za-z0-9_][A-Za-z0-9_/]*\/$/.test(draft.framePrefix)) {
    return "Frame prefix must be empty or use only ASCII letters, digits, '_' and '/', ending in '/'.";
  }
  const renamed = renameComponentRows(draft.entry.components, draft.ids);
  const rows = draft.entry.kind === 'tool' ? withMountRobot(renamed, draft.mountOn) : renamed;
  return rows.map((row) => {
    if (row.component_id !== mainId) return row;
    if (draft.entry.kind === 'tool') {
      const mounted = Object.fromEntries(Object.entries(row).filter(([key]) => key !== 'root_pose_in_cell'));
      return { ...mounted, frame_prefix: draft.framePrefix };
    }
    if (draft.entry.kind !== 'robot') return row;
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
 * into the Deployment Draft. Nothing runs until the one confirmed Apply under
 * Deployment, Changes; the Program role feedback shows what each choice binds.
 */
export function AddComponentPanel({ controller, catalog, catalogError, feedback, onAdded }: {
  controller: DeploymentWorkspaceController;
  catalog: ComponentCatalog | null;
  catalogError: string | null;
  feedback: readonly RoleFeedback[];
  /** Called with the added main component's id. */
  onAdded: (componentId: string) => void;
}) {
  const [open, setOpen] = useState(false);
  const [kind, setKind] = useState<ComponentCatalogKind>('robot');
  const [draft, setDraft] = useState<AddDraft | null>(null);
  // Local validation (red, blocks Add until an edit) or a pointer to the owner's refusal (neutral).
  const [error, setError] = useState<{ message: string; refused: boolean } | null>(null);
  const document = controller.workspace?.draft.document ?? null;
  const entries = useMemo(
    () => (catalog?.entries ?? []).filter((entry) => entry.kind === kind),
    [catalog, kind],
  );
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
    const rows = addedComponentRows(draft);
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
          title={unavailable ?? 'Add a robot, tool, I/O device, pose source or AI model to the Deployment Draft.'}
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
        Pick a component that already exists in a ManyForge example Scenario. It is added to the
        Deployment Draft only; review it under Deployment, Changes, then Apply once to bring it up.
      </p>
      {/* Yellow: a failed read blocks nothing else (M55 notice rule). */}
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
        {entries.map((entry) => {
          const fits = rolesFittingEntry(entry, feedback);
          const selected = draft?.entry.entryId === entry.entryId;
          const others = entry.components.map((row) => row.component_id as string)
            .filter((id) => id !== entry.componentId);
          return (
            <button key={entry.entryId} type="button" role="option" aria-selected={selected}
              className={`dpick${selected ? ' selected' : ''}`}
              title={entry.description ?? undefined}
              onClick={() => { setDraft(initialDraft(entry, document, feedback, mounts)); setError(null); }}>
              <span>
                <strong>{entry.componentProfileId}</strong>
                <small>
                  {entry.componentId}
                  {others.length > 0 ? ` + ${others.join(', ')}` : ''}
                  {entry.lane !== null ? ` · ${entry.lane}` : ''}
                  {entry.tags.length > 0 ? ` · ${entry.tags.join(', ')}` : ''}
                </small>
              </span>
              <small>
                {fits.length > 0
                  ? `Fits role ${fits.map((role) => role.roleId).join(', ')}`
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
          {draft.entry.components.map((row) => {
            const original = row.component_id as string;
            const id = draft.ids[original];
            const role = feedback.find((candidate) => candidate.componentId === id);
            return (
              <label key={original}>
                <span>Component id{draft.entry.components.length > 1 ? ` (${original})` : ''}</span>
                <input aria-label={`Component id for ${original}`} value={id} disabled={controller.busy}
                  onChange={(event) => {
                    setDraft({ ...draft, ids: { ...draft.ids, [original]: event.target.value } });
                    setError(null);
                  }} />
                <small>{role !== undefined
                  ? `Binds Program role ${role.roleId}.`
                  : 'No Program role names this id.'}</small>
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
          {(draft.entry.kind === 'robot' || draft.entry.kind === 'tool') && (
            <label>
              <span>TF frame prefix</span>
              <input aria-label="New component frame prefix" value={draft.framePrefix} disabled={controller.busy}
                onChange={(event) => { setDraft({ ...draft, framePrefix: event.target.value }); setError(null); }} />
              <small>Needed when the same model is already in the cell; empty keeps unprefixed frames.</small>
            </label>
          )}
          {draft.entry.kind === 'robot' && (
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
