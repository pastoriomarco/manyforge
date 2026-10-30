import { useEffect, useState } from 'react';

import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { JsonValue } from '../types/api';
import type { JsonObject } from '../utils/poseSourceAuthoring';
import {
  DEADBANDS,
  POSE_APPLY_POLICIES,
  defaultSourceKey,
  effectiveStreamBudgetNs,
  jsonObject,
  keyKind,
  offeredKeyKinds,
  poseLinks,
  poseSourceComponents,
  savePoseLink,
  withEntity,
  type PoseKeyKind,
  type PoseLink,
} from '../utils/poseSourceAuthoring';
import { DraftTextInput } from './common/DraftInputs';

const KEY_KIND_LABELS: Record<PoseKeyKind, string> = {
  simulation_entity: 'Simulation entity',
  detection3d: '3D detection',
};

const SELECTIONS = [
  ['', 'By class or detection id'],
  ['first', 'First detection'],
  ['best_score', 'Best score'],
] as const;

const nonNegative = (text: string) => (
  text.trim() === '' || (Number.isFinite(Number(text)) && Number(text) >= 0) ? null : 'Enter a number ≥ 0.'
);

/**
 * The Object panel's link to a Deployment pose source: one
 * `policies.scene_pose_sources` row of the Deployment Draft for this object.
 * A source whose key is not complete yet (an empty entity path) stays local
 * until the key is entered.
 */
export function ObjectPoseSourceCard({ controller, sceneResourceId }: {
  controller: DeploymentWorkspaceController;
  sceneResourceId: string;
}) {
  const document = controller.workspace?.draft.document ?? null;
  const components = poseSourceComponents(document);
  const stored = poseLinks(document).find((row) => row.scene_resource_id === sceneResourceId) ?? null;
  const [pending, setPending] = useState<PoseLink | null>(null);
  useEffect(() => { setPending(null); }, [sceneResourceId]);
  const link = pending ?? stored;
  const component = components.find((item) => item.componentId === link?.source_id);
  const kinds = offeredKeyKinds(component);
  const kind = link === null ? null : keyKind(link);
  const disabled = !controller.mutationAvailable || controller.busy || controller.loading;

  const commit = async (next: PoseLink | null) => {
    const simulation = jsonObject(next?.source_key.simulation_entity);
    if (next !== null && (keyKind(next) === null
      || (simulation !== null && (!simulation.entity || !simulation.source_header_frame_id)))) {
      setPending(next);
      return;
    }
    // A refusal shows once, in the yellow Deployment notice row.
    if (await savePoseLink(controller, sceneResourceId, next) !== null) setPending(null);
  };
  const withKind = (base: PoseLink, next: PoseKeyKind | undefined): PoseLink => ({
    ...base, source_key: next === undefined ? {} : defaultSourceKey(next),
  });
  const chooseSource = (sourceId: string) => {
    if (sourceId === '') { setPending(null); void commit(null); return; }
    const offered = offeredKeyKinds(components.find((item) => item.componentId === sourceId));
    const base: PoseLink = link === null
      ? { scene_resource_id: sceneResourceId, source_id: sourceId, source_key: {}, apply_policy: 'observe_only' }
      : { ...link, source_id: sourceId };
    void commit(kind !== null && offered.includes(kind) ? base : withKind(base, offered[0]));
  };
  const setKey = (next: JsonObject) => {
    if (link === null || kind === null) return;
    void commit({ ...link, source_key: { [kind]: next } });
  };
  const keyField = (field: string, value: JsonValue) => setKey({ ...key, [field]: value });
  const deadband = (field: string, scale: number, text: string) => {
    if (link === null) return;
    const next: PoseLink = { ...link };
    const value = Number(text) / scale;
    if (value > 0) next[field] = value;
    else delete next[field];
    void commit(next);
  };
  const key = jsonObject(kind === null ? undefined : link?.source_key[kind]) ?? {};
  const budgetNs = link === null ? null : effectiveStreamBudgetNs(document, component, sceneResourceId);

  return (
    <section className="dcard" aria-label="Object pose source">
      <h4>Pose source</h4>
      {document === null ? (
        <p className="muted">Open the Deployment Draft to link this object to a pose source.</p>
      ) : (
        <>
          <p className="field-help">An observed pose updates this object in the runtime Scene used for planning, collision checks and display. The link is stored in the Deployment Draft; Save persists it, Apply activates it.</p>
          <label>
            <span>Source</span>
            <select aria-label="Pose source" value={link?.source_id ?? ''} disabled={disabled}
              onChange={(event) => chooseSource(event.target.value)}>
              <option value="">None — Program pose only</option>
              {components.map((item) => (
                <option key={item.componentId} value={item.componentId}>{item.componentId}</option>
              ))}
              {link !== null && component === undefined && (
                <option value={link.source_id}>{link.source_id} (not a pose source)</option>
              )}
            </select>
          </label>
          {link !== null && (
            <>
              {component !== undefined && kinds.length === 0 && (
                <p className="inline-message severity-warning">This source has no simulation or detection3d block; configure it in the Deployment.</p>
              )}
              {kinds.length > 1 && (
                <label>
                  <span>Key</span>
                  <select aria-label="Pose source key kind" value={kind ?? ''} disabled={disabled}
                    onChange={(event) => void commit(withKind(link, event.target.value as PoseKeyKind))}>
                    {kinds.map((item) => <option key={item} value={item}>{KEY_KIND_LABELS[item]}</option>)}
                  </select>
                </label>
              )}
              {kind === 'simulation_entity' && (
                <label>
                  <span>Entity path</span>
                  <DraftTextInput aria-label="Pose source entity" value={String(key.entity ?? '')}
                    placeholder="/World/object" disabled={disabled}
                    onCommit={(value) => setKey(withEntity(key, value.trim()))} />
                </label>
              )}
              {kind === 'simulation_entity' && (
                <label>
                  <span>Header frame</span>
                  <DraftTextInput aria-label="Pose source header frame" value={String(key.source_header_frame_id ?? '')}
                    placeholder="frame id of the entity pose" disabled={disabled}
                    onCommit={(value) => keyField('source_header_frame_id', value.trim())} />
                </label>
              )}
              {kind === 'simulation_entity' && (!key.entity || !key.source_header_frame_id) && (
                <p className="muted">Enter the entity path and header frame to store the link.</p>
              )}
              {kind === 'detection3d' && (
                <>
                  <div className="program-template-grid dgrid">
                    <label>
                      <span>Class id</span>
                      <DraftTextInput aria-label="Pose source class id" value={String(key.class_id ?? '')}
                        disabled={disabled} onCommit={(value) => keyField('class_id', value)} />
                    </label>
                    <label>
                      <span>Detection id</span>
                      <DraftTextInput aria-label="Pose source detection id" value={String(key.detection_id ?? '')}
                        disabled={disabled} onCommit={(value) => keyField('detection_id', value)} />
                    </label>
                    <label>
                      <span>Minimum confidence</span>
                      <DraftTextInput aria-label="Pose source minimum confidence" type="number" step="any"
                        value={Number(key.confidence_min ?? 0)} disabled={disabled} validate={nonNegative}
                        onCommit={(value) => keyField('confidence_min', Number(value))} />
                    </label>
                    <label>
                      <span>Max position variance (m²)</span>
                      <DraftTextInput aria-label="Pose source maximum position variance" type="number" step="any"
                        value={Number(key.max_position_variance_m2 ?? 0)} disabled={disabled} validate={nonNegative}
                        onCommit={(value) => keyField('max_position_variance_m2', Number(value))} />
                    </label>
                  </div>
                  <label>
                    <span>Selection</span>
                    <select aria-label="Pose source selection" value={String(key.selection ?? '')} disabled={disabled}
                      onChange={(event) => keyField('selection', event.target.value)}>
                      {SELECTIONS.map(([value, label]) => <option key={value} value={value}>{label}</option>)}
                    </select>
                  </label>
                </>
              )}
              <label>
                <span>Apply policy</span>
                <select aria-label="Pose apply policy" value={link.apply_policy} disabled={disabled}
                  onChange={(event) => void commit({ ...link, apply_policy: event.target.value })}>
                  {POSE_APPLY_POLICIES.map(([value, label]) => <option key={value} value={value}>{label}</option>)}
                </select>
              </label>
              <div className="program-template-grid dgrid">
                {DEADBANDS.map(([field, unit, scale]) => (
                  <label key={field}>
                    <span>Deadband ({unit})</span>
                    <DraftTextInput aria-label={`Pose deadband ${unit}`} type="number" min={0} step="any"
                      placeholder="exact" disabled={disabled} validate={nonNegative}
                      value={typeof link[field] === 'number' ? Number(((link[field] as number) * scale).toPrecision(12)) : ''}
                      onCommit={(value) => deadband(field, scale, value)} />
                  </label>
                ))}
              </div>
              <p className="muted">
                Lifetime: {budgetNs === null ? 'not declared' : `${budgetNs / 1e9} s`}. It is the stream budget
                of <code>poses/{sceneResourceId}</code>, set in the Deployment stream settings.
              </p>
            </>
          )}
        </>
      )}
    </section>
  );
}
