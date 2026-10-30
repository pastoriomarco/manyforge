import { useEffect, useRef, useState } from 'react';
import type { ReactNode } from 'react';

import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { JsonValue } from '../types/api';
import { isIoProviderComponent } from '../utils/ioSignalConfiguration';
import {
  jsonObject,
  keySummary,
  poseLinks,
  poseSourceComponents,
  savePoseLink,
  withEntity,
  type JsonObject,
  type PoseLink,
  type PoseSourceComponent,
} from '../utils/poseSourceAuthoring';
import { DraftTextInput } from './common/DraftInputs';
import { LifecycleBudgetField } from './LifecycleBudgetField';
import type { FocusedEditorScope } from './DeploymentFocusedEditor';
import { pneumaticGripperDrafts } from '../utils/pneumaticGripperAuthoring';

const text = (value: JsonValue | undefined) => (typeof value === 'string' ? value : '');
const numbers = (value: JsonValue | undefined) => (Array.isArray(value) ? value.join(', ') : '');
const parseNumbers = (value: string) => value.split(',').map((item) => Number(item.trim()));
const numberList = (count: number) => (value: string) => {
  const parsed = parseNumbers(value);
  return parsed.length === count && parsed.every(Number.isFinite) ? null : `Enter ${count} comma-separated numbers.`;
};
const range = (minimum: number, maximum: number) => (value: string) => {
  const parsed = Number(value);
  return value.trim() !== '' && parsed >= minimum && parsed <= maximum
    ? null : `Enter a number from ${minimum} to ${maximum}.`;
};

interface RelativeReference {
  controllable_component_id: string;
  command_target_id: string;
  gripper_link: string;
  entity: string;
  link: string;
  source_header_frame_id: string;
}

interface RelativeReferenceDraft {
  key: string;
  value: RelativeReference;
}

interface RelativeReferenceDraftState {
  configurationKey: string;
  rows: RelativeReferenceDraft[];
}

function relativeReferences(value: JsonValue | undefined): RelativeReference[] {
  if (!Array.isArray(value)) return [];
  return value.flatMap((item) => {
    const row = jsonObject(item);
    return row === null ? [] : [{
      controllable_component_id: text(row.controllable_component_id),
      command_target_id: text(row.command_target_id),
      gripper_link: text(row.gripper_link),
      entity: text(row.entity),
      link: text(row.link),
      source_header_frame_id: text(row.source_header_frame_id),
    }];
  });
}

function completeRelativeReferences(
  rows: readonly RelativeReferenceDraft[],
  gripperTargets: readonly { componentId: string; targetId: string }[],
): boolean {
  const identities = rows.map(({ value }) => JSON.stringify([
    value.controllable_component_id.trim(),
    value.command_target_id.trim(),
  ]));
  return rows.every(({ value }) => (
    value.controllable_component_id.trim() !== ''
    && value.command_target_id.trim() !== ''
    && gripperTargets.some((target) => (
      target.componentId === value.controllable_component_id.trim()
      && target.targetId === value.command_target_id.trim()
    ))
  ))
    && new Set(identities).size === identities.length
    && rows.every(({ value }) => [
      value.entity,
      value.link,
      value.source_header_frame_id,
    ].every((field) => field.trim() !== ''));
}

function Field({ label, prefix, value, disabled, validate, type, onCommit }: {
  label: string;
  prefix: string;
  value: string | number;
  disabled: boolean;
  validate?: (value: string) => string | null;
  type?: string;
  onCommit: (value: string) => void;
}) {
  return (
    <label className="deployment-focused-field">
      <span>{label}</span>
      <DraftTextInput aria-label={`${prefix} ${label}`} value={value} type={type} step="any"
        disabled={disabled} validate={validate} onCommit={onCommit} />
    </label>
  );
}

/**
 * An optional block of the component configuration. Turning it on stages its
 * empty fields locally; the block is written once its required fields are set.
 */
function OptionalBlock({ label, prefix, stored, initial, complete, disabled, onWrite, children }: {
  label: string;
  prefix: string;
  stored: JsonObject | null;
  initial: JsonObject;
  complete: (value: JsonObject) => boolean;
  disabled: boolean;
  onWrite: (value: JsonObject | null) => void;
  children: (value: JsonObject, set: (next: JsonObject) => void) => ReactNode;
}) {
  const [staged, setStaged] = useState<JsonObject | null>(null);
  const value = staged ?? stored;
  const set = (next: JsonObject | null) => {
    if (next !== null && !complete(next)) { setStaged(next); return; }
    setStaged(null);
    onWrite(next);
  };
  return (
    <fieldset className="deployment-pose-source-block">
      <label className="toggle-control">
        <input type="checkbox" aria-label={`${prefix} ${label}`} checked={value !== null} disabled={disabled}
          onChange={(event) => set(event.target.checked ? stored ?? initial : null)} />
        {label}
      </label>
      {value !== null && children(value, set)}
      {value !== null && !complete(value) && <p className="muted">Fill in the required fields to store this block.</p>}
    </fieldset>
  );
}

function PoseSourceComponentForm({ controller, component, robotIds, gripperTargets, links }: {
  controller: DeploymentWorkspaceController;
  component: PoseSourceComponent;
  robotIds: string[];
  gripperTargets: Array<{ componentId: string; targetId: string }>;
  links: PoseLink[];
}) {
  const configuration = component.runtimeConfiguration;
  const prefix = component.componentId;
  const gripperComponentIds = [...new Set(gripperTargets.map((target) => target.componentId))];
  const nextRelativeReferenceKey = useRef(0);
  const disabled = !controller.mutationAvailable || controller.busy || controller.loading;
  const write = async (next: JsonObject) => {
    // A refusal shows once, in the yellow Deployment notice row.
    await controller.mutate(
      ['components', component.index, 'configuration', 'runtime_configuration'], 'set', next);
  };
  const block = (name: string) => (value: JsonObject | null) => {
    const next = { ...configuration };
    if (value === null) delete next[name];
    else next[name] = value;
    void write(next);
  };
  const member = (parent: JsonObject, set: (next: JsonObject) => void, name: string) => (value: JsonObject | null) => {
    const next = { ...parent };
    if (value === null) delete next[name];
    else next[name] = value;
    set(next);
  };
  const budgetNs = typeof configuration.stream_budget_ns === 'number' ? configuration.stream_budget_ns : null;
  const overlay = jsonObject(configuration.robot_overlay);
  const simulation = jsonObject(configuration.simulation) ?? {};
  const storedReferences = relativeReferences(simulation.relative_references);
  const hasLegacyRelativeReference = Object.prototype.hasOwnProperty.call(
    simulation,
    'relative_reference',
  );
  const relativeReferencesKey = JSON.stringify(simulation.relative_references ?? null);
  const [relativeReferencesDraft, setRelativeReferencesDraft] = useState<RelativeReferenceDraftState | null>(null);
  useEffect(() => { setRelativeReferencesDraft(null); }, [component.componentId, relativeReferencesKey]);
  const relativeReferenceRows = relativeReferencesDraft?.configurationKey === relativeReferencesKey
    ? relativeReferencesDraft.rows
    : storedReferences.map((value, index) => ({
      key: `stored-${index}-${value.controllable_component_id}-${value.command_target_id}`,
      value,
    }));
  const addRelativeReference = () => {
    const rows = [
      ...relativeReferenceRows,
      {
        key: `pending-${nextRelativeReferenceKey.current++}`,
        value: {
          controllable_component_id: '',
          command_target_id: '',
          gripper_link: '',
          entity: '',
          link: '',
          source_header_frame_id: '',
        },
      },
    ];
    setRelativeReferencesDraft({ configurationKey: relativeReferencesKey, rows });
  };

  return (
    <section className="deployment-pose-source" aria-label={`${prefix} pose source`}>
      <header><strong>{prefix}</strong></header>
      <LifecycleBudgetField controller={controller} componentIndex={component.index}
        componentId={component.componentId} disabled={disabled} />
      <Field label="Stream budget (s)" prefix={prefix} type="number" disabled={disabled}
        value={budgetNs === null ? '' : budgetNs / 1e9} validate={range(1e-9, 60)}
        onCommit={(value) => { void write({ ...configuration, stream_budget_ns: Math.round(Number(value) * 1e9) }); }} />
      <p className="field-help">Default lifetime of every pose stream of this source; one object&apos;s stream (<code>poses/&lt;object&gt;</code>) can override it in the stream settings.</p>
      <OptionalBlock label="Simulation entities" prefix={prefix} disabled={disabled}
        stored={jsonObject(configuration.simulation)}
        initial={{ poll_period_ms: 250, source_coordinate_frame_id: '', target_frame_id: '' }}
        complete={(value) => text(value.source_coordinate_frame_id) !== '' && text(value.target_frame_id) !== ''}
        onWrite={block('simulation')}>
        {(simulation, set) => {
          const commitRelativeReferences = (rows: readonly RelativeReferenceDraft[]) => {
            const nextSimulation = { ...simulation };
            if (rows.length === 0) {
              delete nextSimulation.relative_references;
            } else if (completeRelativeReferences(rows, gripperTargets)) {
              nextSimulation.relative_references = rows.map(({ value }) => ({
                controllable_component_id: value.controllable_component_id.trim(),
                command_target_id: value.command_target_id.trim(),
                ...(value.gripper_link.trim() === ''
                  ? {}
                  : { gripper_link: value.gripper_link.trim() }),
                entity: value.entity.trim(),
                link: value.link.trim(),
                source_header_frame_id: value.source_header_frame_id.trim(),
              }));
              delete nextSimulation.relative_reference;
            } else {
              return;
            }
            set(nextSimulation);
          };
          const updateRelativeReference = (
            index: number,
            field: keyof RelativeReference,
            value: string,
          ) => {
            const rows = relativeReferenceRows.map((row, rowIndex) => rowIndex === index
              ? {
                  ...row,
                  value: {
                    ...row.value,
                    [field]: value,
                    ...(field === 'controllable_component_id'
                      && value !== row.value.controllable_component_id
                      ? { command_target_id: '', gripper_link: '' }
                      : {}),
                    ...(field === 'command_target_id' && value !== row.value.command_target_id
                      ? { gripper_link: '' }
                      : {}),
                  },
                }
              : row);
            setRelativeReferencesDraft({ configurationKey: relativeReferencesKey, rows });
            commitRelativeReferences(rows);
          };
          const removeRelativeReference = (index: number) => {
            const rows = relativeReferenceRows.filter((_, rowIndex) => rowIndex !== index);
            setRelativeReferencesDraft({ configurationKey: relativeReferencesKey, rows });
            commitRelativeReferences(rows);
          };
          return (
          <>
            <Field label="Poll period (ms)" prefix={prefix} type="number" disabled={disabled}
              value={Number(simulation.poll_period_ms ?? 250)} validate={range(20, 60000)}
              onCommit={(value) => set({ ...simulation, poll_period_ms: Math.round(Number(value)) })} />
            <Field label="Source frame" prefix={prefix} disabled={disabled}
              value={text(simulation.source_coordinate_frame_id)}
              onCommit={(value) => set({ ...simulation, source_coordinate_frame_id: value.trim() })} />
            <Field label="Target frame" prefix={prefix} disabled={disabled} value={text(simulation.target_frame_id)}
              onCommit={(value) => set({ ...simulation, target_frame_id: value.trim() })} />
            <fieldset className="deployment-pose-source-block" aria-label={`${prefix} relative references`}>
              <legend>Relative references by gripper target</legend>
              {hasLegacyRelativeReference && (
                <p className="field-help" role="status">
                  legacy single reference: migrated at Apply only if the cell has exactly one gripper target; otherwise set per-target references
                </p>
              )}
              <p className="field-help">
                Match the attached object&apos;s robot and configured gripper target to the rigid body pose source.
                Use the gripper base body link for the measured body, never a finger; the owner validates it against the model.
              </p>
              {relativeReferenceRows.map((row, index) => {
                const rowPrefix = `${prefix} relative reference ${index + 1}`;
                const selectedTargetsElsewhere = new Set(relativeReferenceRows.flatMap((candidate, candidateIndex) => (
                  candidateIndex !== index
                  && candidate.value.controllable_component_id !== ''
                  && candidate.value.command_target_id !== ''
                    ? [JSON.stringify([
                        candidate.value.controllable_component_id,
                        candidate.value.command_target_id,
                      ])]
                    : []
                )));
                const targetOptions = gripperTargets.filter((target) => (
                  target.componentId === row.value.controllable_component_id
                ));
                return (
                  <div className="deployment-pose-source-block" key={row.key}>
                    <label className="deployment-focused-field">
                      <span>Robot component</span>
                      <select
                        aria-label={`${rowPrefix} robot component`}
                        value={row.value.controllable_component_id}
                        disabled={disabled}
                        onChange={(event) => updateRelativeReference(
                          index, 'controllable_component_id', event.target.value,
                        )}
                      >
                        <option value="">Select a robot</option>
                        {gripperComponentIds.map((id) => (
                          <option key={id} value={id}>{id}</option>
                        ))}
                        {row.value.controllable_component_id !== ''
                          && !gripperComponentIds.includes(row.value.controllable_component_id) && (
                          <option value={row.value.controllable_component_id}>
                            {row.value.controllable_component_id} (no configured gripper target in this Draft)
                          </option>
                        )}
                      </select>
                    </label>
                    <label className="deployment-focused-field">
                      <span>Gripper command target</span>
                      <select
                        aria-label={`${rowPrefix} gripper command target`}
                        value={row.value.command_target_id}
                        disabled={disabled || row.value.controllable_component_id === ''}
                        onChange={(event) => updateRelativeReference(
                          index, 'command_target_id', event.target.value,
                        )}
                      >
                        <option value="">Select a configured gripper target</option>
                        {targetOptions.map((target) => {
                          const identity = JSON.stringify([target.componentId, target.targetId]);
                          return (
                            <option
                              key={identity}
                              value={target.targetId}
                              disabled={target.targetId !== row.value.command_target_id
                                && selectedTargetsElsewhere.has(identity)}
                            >{target.targetId}</option>
                          );
                        })}
                        {row.value.command_target_id !== ''
                          && !targetOptions.some((target) => target.targetId === row.value.command_target_id) && (
                          <option value={row.value.command_target_id}>
                            {row.value.command_target_id} (not in this Draft&apos;s gripper targets)
                          </option>
                        )}
                      </select>
                    </label>
                    <Field label="Gripper base link (optional)" prefix={rowPrefix} disabled={disabled}
                      value={row.value.gripper_link}
                      onCommit={(value) => updateRelativeReference(index, 'gripper_link', value.trim())} />
                    <p className="field-help">
                      Leave the base link blank for the owner to derive it from this target; any explicit link is validated against the model.
                    </p>
                    <Field label="Measured entity" prefix={rowPrefix} disabled={disabled}
                      value={row.value.entity}
                      onCommit={(value) => updateRelativeReference(index, 'entity', value.trim())} />
                    <Field label="Measured body link" prefix={rowPrefix} disabled={disabled}
                      value={row.value.link}
                      onCommit={(value) => updateRelativeReference(index, 'link', value.trim())} />
                    <Field label="Source header frame" prefix={rowPrefix} disabled={disabled}
                      value={row.value.source_header_frame_id}
                      onCommit={(value) => updateRelativeReference(
                        index, 'source_header_frame_id', value.trim(),
                      )} />
                    <button
                      type="button"
                      aria-label={`Remove ${rowPrefix}`}
                      disabled={disabled}
                      onClick={() => removeRelativeReference(index)}
                    >Remove reference</button>
                  </div>
                );
              })}
              <button
                type="button"
                aria-label={`${prefix} add gripper target reference`}
                disabled={disabled || gripperTargets.length === 0}
                onClick={addRelativeReference}
              >Add gripper target reference</button>
              {gripperTargets.length === 0 && (
                <p className="muted">Configure a gripper target in this Deployment before adding a reference.</p>
              )}
              {relativeReferenceRows.length > 0
                && !completeRelativeReferences(relativeReferenceRows, gripperTargets) && (
                <p className="muted">Complete all required fields, select a configured target, and use a unique robot plus command-target pair for each reference before it is stored.</p>
              )}
            </fieldset>
          </>
          );
        }}
      </OptionalBlock>
      <OptionalBlock label="3D detections" prefix={prefix} disabled={disabled}
        stored={jsonObject(configuration.detection3d)}
        initial={{ topic: '', target_frame_id: '', fallback_frame_id: '' }}
        complete={(value) => text(value.topic).length >= 2 && text(value.target_frame_id) !== ''}
        onWrite={block('detection3d')}>
        {(detection, set) => (
          <>
            <Field label="Detection topic" prefix={prefix} disabled={disabled} value={text(detection.topic)}
              onCommit={(value) => set({ ...detection, topic: value.trim() })} />
            <Field label="Detection target frame" prefix={prefix} disabled={disabled}
              value={text(detection.target_frame_id)}
              onCommit={(value) => set({ ...detection, target_frame_id: value.trim() })} />
            <Field label="Detection fallback frame" prefix={prefix} disabled={disabled}
              value={text(detection.fallback_frame_id)}
              onCommit={(value) => set({ ...detection, fallback_frame_id: value.trim() })} />
          </>
        )}
      </OptionalBlock>
      <OptionalBlock label="Robot overlay" prefix={prefix} disabled={disabled} stored={overlay}
        initial={{ robot_component_id: robotIds[0] ?? '' }}
        complete={(value) => text(value.robot_component_id) !== ''}
        onWrite={block('robot_overlay')}>
        {(robot, set) => (
          <>
            <label className="deployment-focused-field">
              <span>Robot component</span>
              <select aria-label={`${prefix} Robot component`} value={text(robot.robot_component_id)}
                disabled={disabled} onChange={(event) => set({ ...robot, robot_component_id: event.target.value })}>
                {!robotIds.includes(text(robot.robot_component_id)) && (
                  <option value={text(robot.robot_component_id)}>{text(robot.robot_component_id) || 'Select a robot'}</option>
                )}
                {robotIds.map((id) => <option key={id} value={id}>{id}</option>)}
              </select>
            </label>
            <OptionalBlock label="Measured joint state" prefix={prefix} disabled={disabled}
              stored={jsonObject(robot.joint_state)} initial={{ topic: '', required_joint_names: [] }}
              complete={(value) => text(value.topic).length >= 2
                && Array.isArray(value.required_joint_names) && value.required_joint_names.length > 0}
              onWrite={member(robot, set, 'joint_state')}>
              {(joints, setJoints) => (
                <>
                  <Field label="Joint state topic" prefix={prefix} disabled={disabled} value={text(joints.topic)}
                    onCommit={(value) => setJoints({ ...joints, topic: value.trim() })} />
                  <Field label="Joint names" prefix={prefix} disabled={disabled}
                    value={numbers(joints.required_joint_names)}
                    onCommit={(value) => setJoints({
                      ...joints,
                      required_joint_names: value.split(',').map((item) => item.trim()).filter(Boolean),
                    })} />
                </>
              )}
            </OptionalBlock>
            <OptionalBlock label="Measured TCP entity" prefix={prefix} disabled={disabled}
              stored={jsonObject(robot.tcp_entity)}
              initial={{ entity: '', source_header_frame_id: '', local_position_xyz: [0, 0, 0],
                local_orientation_xyzw: [0, 0, 0, 1] }}
              complete={(value) => text(value.entity) !== '' && text(value.source_header_frame_id) !== ''}
              onWrite={member(robot, set, 'tcp_entity')}>
              {(tcp, setTcp) => (
                <>
                  <Field label="TCP entity" prefix={prefix} disabled={disabled} value={text(tcp.entity)}
                    onCommit={(value) => setTcp(withEntity(tcp, value.trim()))} />
                  <Field label="TCP header frame" prefix={prefix} disabled={disabled}
                    value={text(tcp.source_header_frame_id)}
                    onCommit={(value) => setTcp({ ...tcp, source_header_frame_id: value.trim() })} />
                  <Field label="TCP local position (m)" prefix={prefix} disabled={disabled}
                    value={numbers(tcp.local_position_xyz)} validate={numberList(3)}
                    onCommit={(value) => setTcp({ ...tcp, local_position_xyz: parseNumbers(value) })} />
                  <Field label="TCP local orientation (xyzw)" prefix={prefix} disabled={disabled}
                    value={numbers(tcp.local_orientation_xyzw)} validate={numberList(4)}
                    onCommit={(value) => setTcp({ ...tcp, local_orientation_xyzw: parseNumbers(value) })} />
                </>
              )}
            </OptionalBlock>
          </>
        )}
      </OptionalBlock>
      <h5>Objects using this source</h5>
      {links.length === 0 ? <p className="muted">None. Link an object on its Object panel.</p> : (
        <ul aria-label={`${prefix} linked objects`}>
          {links.map((link) => (
            <li key={link.scene_resource_id}>
              {link.scene_resource_id} · {keySummary(link)} · {link.apply_policy}
            </li>
          ))}
        </ul>
      )}
    </section>
  );
}

/**
 * The Deployment's pose-source components: their own fields, the objects
 * linked to each (links are edited on the Object panel), and links whose
 * object is not in the current Program scene (orphans), which can be removed.
 * One component's page shows only its form; the cell settings only the orphans.
 */
export function PoseSourceComponentSection({ controller, programSceneIds, scope = 'all' }: {
  controller: DeploymentWorkspaceController;
  /** The Program Draft's scene object ids; null without a Program (no orphan list). */
  programSceneIds: readonly string[] | null;
  scope?: FocusedEditorScope;
}) {
  const document = controller.workspace?.draft.document ?? null;
  const components = scope === 'cell' ? [] : poseSourceComponents(document).filter((component) => (
    typeof scope !== 'object' || component.componentId === scope.componentId));
  const links = poseLinks(document);
  const orphans = programSceneIds === null || typeof scope === 'object'
    ? [] : links.filter((link) => !programSceneIds.includes(link.scene_resource_id));
  if (components.length === 0 && orphans.length === 0) return null;
  const robotIds = (Array.isArray(document?.components) ? document.components : []).flatMap((value) => {
    const item = jsonObject(value);
    const id = item?.component_id;
    return typeof id === 'string' && !isIoProviderComponent(value)
      && !['pose_source', 'ai_model'].includes(String(jsonObject(item?.configuration)?.runtime_kind))
      ? [id] : [];
  });
  const gripperTargets = [...new Map(pneumaticGripperDrafts(document).map((target) => {
    const value = { componentId: target.componentId, targetId: target.targetId };
    return [JSON.stringify([value.componentId, value.targetId]), value] as const;
  })).values()];
  const disabled = !controller.mutationAvailable || controller.busy || controller.loading;
  const remove = async (sceneResourceId: string) => {
    // A refusal shows once, in the yellow Deployment notice row.
    await savePoseLink(controller, sceneResourceId, null);
  };
  return (
    <details className="dcard">
      <summary>Pose sources</summary>
      <div className="deployment-focused-body">
        {components.map((component) => (
          <PoseSourceComponentForm key={component.componentId} controller={controller} component={component}
            robotIds={robotIds} gripperTargets={gripperTargets}
            links={links.filter((link) => link.source_id === component.componentId)} />
        ))}
        {orphans.length > 0 && (
          <section aria-label="Pose links to objects not in the Program">
            <h5>Links to objects not in the Program</h5>
            <p className="field-help">These links are kept and ignored at runtime while the Program has no such object.</p>
            <ul>
              {orphans.map((link) => (
                <li key={link.scene_resource_id}>
                  {link.scene_resource_id} · {link.source_id}{' '}
                  <button type="button" disabled={disabled} aria-label={`Remove pose link of ${link.scene_resource_id}`}
                    onClick={() => { void remove(link.scene_resource_id); }}>Remove</button>
                </li>
              ))}
            </ul>
          </section>
        )}
        <p className="field-help">A pose source is a model-free component that publishes object poses from the simulator or 3D detections, and optionally the measured robot joint state and TCP for the viewer. Objects are linked to it on their Object panel.</p>
      </div>
    </details>
  );
}
