import { useEffect, useState } from 'react';

import { cellApplyErrorText, type DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import { draftAttachments } from '../state/kinematicIndex';
import { deploymentActionUnavailableReason } from '../state/documentStates';
import { busyLabel, busyReason } from '../state/busyOperation';
import type { JsonValue } from '../types/api';
import type { ComponentProfileFacts } from '../types/componentCatalog';
import type { ApplyPreview } from '../types/components';
import { draftComponents, mountCandidates, type ComponentManagerRow } from '../utils/componentManager';
import { rpyDegToXyzw, xyzwToRpyDeg } from '../utils/orientation';
import { ApplyImpact } from './ApplyImpact';

type Row = Record<string, JsonValue>;
type Triple = [string, string, string];

function object(value: JsonValue | undefined): Row | null {
  return value !== null && value !== undefined && typeof value === 'object' && !Array.isArray(value)
    ? value : null;
}

function triple(values: readonly unknown[] | undefined, fallback: number): Triple {
  return [0, 1, 2].map((index) => String(values?.[index] ?? fallback)) as Triple;
}

function numbers(values: Triple): [number, number, number] | null {
  const parsed = values.map((value) => (value.trim() === '' ? Number.NaN : Number(value)));
  return parsed.every(Number.isFinite) ? parsed as [number, number, number] : null;
}

/** Base position (m) and orientation (roll/pitch/yaw, degrees) of `root_pose_in_cell`. */
function BasePoseFields({ componentId, index, pose, controller, disabled }: {
  componentId: string;
  index: number;
  pose: Row | null;
  controller: DeploymentWorkspaceController;
  disabled: boolean;
}) {
  const positionValue = pose?.position_m;
  const orientationValue = pose?.orientation_xyzw;
  const position = Array.isArray(positionValue) ? positionValue : undefined;
  const orientation = Array.isArray(orientationValue) ? xyzwToRpyDeg(orientationValue) : undefined;
  const source = JSON.stringify([position ?? null, orientation ?? null]);
  const [xyz, setXyz] = useState<Triple>(() => triple(position, 0));
  const [rpy, setRpy] = useState<Triple>(() => triple(orientation, 0));
  const [error, setError] = useState<string | null>(null);
  useEffect(() => {
    setXyz(triple(position, 0));
    setRpy(triple(orientation, 0));
    setError(null);
    // The Draft's value changed: show it.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [source]);
  const commit = async () => {
    const positionM = numbers(xyz);
    const angles = numbers(rpy);
    const xyzw = angles === null ? undefined : rpyDegToXyzw(angles);
    if (positionM === null || xyzw === undefined) {
      setError('Position and orientation must be finite numbers.');
      return;
    }
    await controller.mutate(['components', index, 'root_pose_in_cell'], 'set',
      { position_m: positionM, orientation_xyzw: xyzw });
  };
  const field = (label: string, values: Triple, set: (next: Triple) => void, axis: number, unit: string) => (
    <label key={label}>
      <span>{label} ({unit})</span>
      <input aria-label={`${componentId} base ${label}`} inputMode="decimal" value={values[axis]}
        disabled={disabled}
        onChange={(event) => {
          const next = [...values] as Triple;
          next[axis] = event.target.value;
          set(next);
          setError(null);
        }} />
    </label>
  );
  return (
    <fieldset className="dgrid" aria-label={`${componentId} base pose`}>
      <legend>Base pose in the cell</legend>
      {['x', 'y', 'z'].map((axis, at) => field(axis, xyz, setXyz, at, 'm'))}
      {['roll', 'pitch', 'yaw'].map((axis, at) => field(axis, rpy, setRpy, at, '°'))}
      {error !== null && <p className="inline-message severity-error" role="alert">{error}</p>}
      <div className="dactions">
        <button type="button" disabled={disabled} onClick={() => { void commit(); }}>Update base pose</button>
      </div>
    </fieldset>
  );
}

type ExclusionPair = { child_link: string; parent_link: string };

function exclusionPairs(value: JsonValue | undefined): ExclusionPair[] {
  return (Array.isArray(value) ? value : []).flatMap((item) => {
    const childLink = object(item)?.child_link;
    const parentLink = object(item)?.parent_link;
    return typeof childLink === 'string' && typeof parentLink === 'string'
      ? [{ child_link: childLink, parent_link: parentLink }] : [];
  });
}

const exclusionText = (pairs: readonly ExclusionPair[]) =>
  pairs.map((pair) => `${pair.child_link} ${pair.parent_link}`).join('\n');

/** One pair per line, `tool_link parent_link`; null when a line is not two names. */
function parseExclusions(text: string): ExclusionPair[] | null {
  const pairs = new Map<string, ExclusionPair>();
  for (const line of text.split('\n')) {
    const names = line.split(/[\s,]+/).filter(Boolean);
    if (names.length === 0) continue;
    if (names.length !== 2) return null;
    pairs.set(names.join(' '), { child_link: names[0], parent_link: names[1] });
  }
  // The compiler's order: by tool link, then parent link.
  const order = (a: string, b: string) => (a < b ? -1 : a > b ? 1 : 0);
  return [...pairs.values()].sort((a, b) => (
    order(a.child_link, b.child_link) || order(a.parent_link, b.parent_link)));
}

/** The mount's arm-tool link pairs that never touch, so planning skips them. */
function CollisionExclusionsField({ componentId, pairs, disabled, write }: {
  componentId: string;
  pairs: readonly ExclusionPair[];
  disabled: boolean;
  write: (pairs: ExclusionPair[]) => Promise<unknown>;
}) {
  const source = exclusionText(pairs);
  const [text, setText] = useState(source);
  const [error, setError] = useState<string | null>(null);
  useEffect(() => {
    setText(source);
    setError(null);
  }, [source]);
  const update = async () => {
    const parsed = parseExclusions(text);
    if (parsed === null) {
      setError('Each line must name two links: the tool link, then the parent link.');
      return;
    }
    setError(null);
    await write(parsed);
  };
  return (
    <div className="deployment-focused-field wide">
      <label>
        <span>Collision exclusions with parent</span>
        <textarea aria-label={`${componentId} collision exclusions with parent`} value={text}
          disabled={disabled} onChange={(event) => { setText(event.target.value); setError(null); }} />
      </label>
      <small>One pair per line, tool link then parent link: pairs that never touch, so planning skips them.</small>
      {error !== null && <p className="inline-message severity-error" role="alert">{error}</p>}
      <div className="dactions">
        <button type="button" disabled={disabled || text === source}
          onClick={() => { void update(); }}>Update collision exclusions</button>
      </div>
    </div>
  );
}

/**
 * The Components page editors (plan, "Authoring and Apply"): base position
 * and orientation, mount and primary tool, lane, Remove. Each is a Draft edit;
 * Preview shows the per-component impact and Apply sends it through the one
 * server-side path, after which the affected components come back prepared.
 */
export function ComponentPlacementEditor({ row, controller, profiles, onRemoved }: {
  row: ComponentManagerRow;
  controller: DeploymentWorkspaceController;
  profiles: Readonly<Record<string, ComponentProfileFacts>>;
  onRemoved: () => void;
}) {
  const [preview, setPreview] = useState<ApplyPreview | null>(null);
  const [error, setError] = useState<string | null>(null);
  const document = controller.workspace?.draft.document ?? null;
  const rows = Array.isArray(document?.components) ? document.components : [];
  const index = rows.findIndex((candidate) => object(candidate)?.component_id === row.componentId);
  if (document === null || document.api_version !== 'manyforge.deployment_intent.v2' || index < 0) return null;
  const component = object(rows[index])!;
  const id = row.componentId;
  const disabled = !controller.mutationAvailable || controller.busy || controller.loading;
  const attachments = draftAttachments(document);
  const mountedAt = attachments.find((attachment) => attachment.child.componentId === id) ?? null;
  const parentIds = new Set(draftComponents(document).map((candidate) => candidate.component_id as string));
  const parents = [...new Set([...mountCandidates(document, profiles),
    ...(mountedAt === null ? [] : [mountedAt.parent.componentId])])].filter((parent) => parentIds.has(parent)).sort();
  const lanes = row.profileId === null ? undefined : profiles[row.profileId]?.lanes;
  const lane = typeof component.lane === 'string' ? component.lane : null;
  const pose = object(component.root_pose_in_cell);
  const applyReason = deploymentActionUnavailableReason(
    controller.workspace, 'apply', controller.documentUnavailableReason,
  );

  const rawAttachments = Array.isArray(document.attachments) ? document.attachments : [];
  const isChildRow = (value: JsonValue) => object(object(value)?.child)?.component_id === id;
  const mountedExclusions = exclusionPairs(object(rawAttachments.find(isChildRow))?.collision_exclusions);
  // The pairs name the parent's links: kept while the parent stays, dropped on a move.
  const writeMount = (parent: string, primary: boolean, exclusions = mountedExclusions) => {
    const kept = parent === mountedAt?.parent.componentId ? exclusions : [];
    const child = mountedAt?.child.interface ?? 'mount';
    const parentInterface = mountedAt?.parent.interface ?? 'flange';
    const next = rawAttachments.filter((value) => !isChildRow(value)).map((value) => {
      // One primary tool per parent interface.
      const other = object(value);
      const otherParent = object(other?.parent ?? null);
      return primary && other !== null && otherParent?.component_id === parent
        && otherParent.interface === parentInterface && other.primary === true
        ? Object.fromEntries(Object.entries(other).filter(([key]) => key !== 'primary')) : value;
    });
    next.push({
      parent: { component_id: parent, interface: parentInterface },
      child: { component_id: id, interface: child },
      ...(primary ? { primary: true } : {}),
      ...(kept.length > 0 ? { collision_exclusions: kept } : {}),
    });
    return controller.mutate(['attachments'], 'set', next);
  };
  const remove = async () => {
    // Its attachment rows go first, so the Draft never names a missing component.
    const kept = rawAttachments.filter((value) => {
      const attachment = object(value);
      return object(attachment?.parent ?? null)?.component_id !== id && !isChildRow(value);
    });
    if (kept.length !== rawAttachments.length
      && await controller.mutate(['attachments'], 'set', kept) === null) return;
    if (await controller.mutate(['components', index], 'remove') !== null) onRemoved();
  };
  const runPreview = async () => {
    setError(null);
    try {
      const next = await controller.previewApply();
      if (next !== null) setPreview(next);
    } catch (reason) {
      setPreview(null);
      setError(cellApplyErrorText(reason, 'Preview failed.'));
    }
  };

  return (
    <section className="dcard" aria-labelledby="component-placement-title">
      <h4 id="component-placement-title">{row.owner === null ? 'Placement and lane' : 'Placement'}</h4>
      {mountedAt === null && (row.kind === 'robot' || pose !== null) && (
        <BasePoseFields componentId={id} index={index} pose={pose} controller={controller} disabled={disabled} />
      )}
      {row.kind === 'tool' && (
        <fieldset className="dgrid" aria-label={`${id} mount`}>
          <legend>Mount</legend>
          <label>
            <span>Mounted on</span>
            <select aria-label={`${id} mounted on`} disabled={disabled || parents.length === 0}
              value={mountedAt?.parent.componentId ?? ''}
              onChange={(event) => { void writeMount(event.target.value, mountedAt?.primary === true); }}>
              {mountedAt === null && <option value="">Not mounted</option>}
              {parents.map((parent) => (
                <option key={parent} value={parent}>{parent}.{mountedAt?.parent.interface ?? 'flange'}</option>
              ))}
            </select>
          </label>
          {mountedAt !== null && (
            <label className="toggle-control">
              <input type="checkbox" aria-label={`${id} primary tool`} checked={mountedAt.primary}
                disabled={disabled}
                onChange={(event) => { void writeMount(mountedAt.parent.componentId, event.target.checked); }} />
              Primary tool of {mountedAt.parent.componentId}.{mountedAt.parent.interface}
            </label>
          )}
          {mountedAt !== null && (
            <CollisionExclusionsField componentId={id} pairs={mountedExclusions} disabled={disabled}
              write={(pairs) => writeMount(mountedAt.parent.componentId, mountedAt.primary, pairs)} />
          )}
        </fieldset>
      )}
      {/* A member runs on its owner's lane (CONTRACTS §16): no lane of its own. */}
      {row.owner === null && <label className="deployment-focused-field">
        <span>Lane</span>
        <select aria-label={`${id} lane`} disabled={disabled || lanes === undefined || lanes.length === 0}
          value={lane ?? ''}
          onChange={(event) => { void controller.mutate(['components', index, 'lane'], 'set', event.target.value); }}>
          {(lane === null || !(lanes ?? []).includes(lane)) && <option value={lane ?? ''}>{lane ?? 'not set'}</option>}
          {(lanes ?? []).map((choice) => <option key={choice} value={choice}>{choice}</option>)}
        </select>
        {lanes === undefined && <small>The component catalog does not list this model's lanes.</small>}
      </label>}
      <div className="dactions">
        {/* A machine member is never removed on its own, only deactivated (AUD-5b revised). */}
        {row.owner === null && (
          <button type="button" disabled={disabled} onClick={() => { void remove(); }}>Remove from Draft</button>
        )}
        <button type="button" disabled={controller.busy} title={busyReason(controller.busyOperation) ?? undefined}
          onClick={() => { void runPreview(); }}>{busyLabel(controller.busyOperation, 'preview', 'Preview')}</button>
        <button type="button" className="primary-action" disabled={applyReason !== null || controller.busy}
          title={applyReason ?? busyReason(controller.busyOperation)
            ?? 'Preview this Draft and confirm its impact; the affected components come back prepared.'}
          onClick={() => { void controller.applyDraft(); }}>
          {busyLabel(controller.busyOperation, ['apply_preview', 'apply'], 'Apply…')}
        </button>
      </div>
      {error !== null && <p className="inline-message severity-warning" role="alert">{error}</p>}
      {preview !== null && <ApplyImpact preview={preview} />}
      <p className="field-help">
        Edits change the Deployment Draft only. Apply disables each replaced or removed component,
        applies the Draft and prepares the replaced ones; Enable them afterwards.
      </p>
    </section>
  );
}
