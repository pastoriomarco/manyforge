import { useId, useState } from 'react';

import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { JsonValue } from '../types/api';
import type { ManualControlMotionDescriptor } from '../types/robotControl';
import type { ComponentFrameTarget, DraftGripperFrames } from '../utils/componentManager';
import {
  componentRowIndex,
  customTcpParentFrames,
  customTcpProblem,
  customTcpsEdit,
  customTcpsOf,
  type CustomTcp,
} from '../utils/customTcps';

/**
 * The "TCPs and frames" card of a component page. Model (URDF/SRDF) frames
 * and descriptor TCPs are shown read-only; custom TCPs (a parent frame of
 * this component plus a position and orientation) are added, edited and
 * deleted here through the Deployment Draft and take effect at Apply.
 */
export function ComponentTcpsCard({
  componentId, controller, frames, gripperFrames, motionDescriptors,
}: {
  componentId: string;
  controller: DeploymentWorkspaceController;
  frames: readonly ComponentFrameTarget[];
  gripperFrames: readonly DraftGripperFrames[];
  motionDescriptors: readonly ManualControlMotionDescriptor[];
}) {
  const document = (controller.workspace?.draft.document ?? null) as Record<string, JsonValue> | null;
  const custom = customTcpsOf(document, componentId);
  const customIds = new Set(custom.map((tcp) => tcp.tcpId));
  // The component's own frames: every Applied frame role except a custom TCP
  // already Applied (a custom TCP is never a parent).
  const componentFrames = [...new Set(frames.flatMap((target) => target.frames.map((frame) => frame.frameRoleId)))]
    .filter((frame) => !customIds.has(frame));
  // The component's TCPs as Robot Control lists them (a tool's appear in its
  // robot's list, on the tool's target).
  const runtimeTcps = motionDescriptors.flatMap((descriptor) => descriptor.tcps)
    .filter((tcp) => tcp.mountedHere && tcp.controllableComponentId === componentId);
  const [editing, setEditing] = useState<{ replacing: string | null; tcp: CustomTcp } | null>(null);
  // Custom TCPs live on the component's intent Draft row: none on a component
  // removed in the Draft or in a compiled Deployment.
  const noDraftRow = componentRowIndex(document, componentId) < 0;
  const noDraftRowTitle = noDraftRow
    ? 'This component has no row in the Deployment Draft (removed, or a compiled Deployment); its custom TCPs cannot be edited.'
    : undefined;
  const disabled = !controller.mutationAvailable || controller.busy || controller.loading;
  const save = async (rows: CustomTcp[]) => {
    const edit = customTcpsEdit(document, componentId, rows);
    if (edit === null) return;
    // A refused edit (null) keeps the form open; the refusal shows as feedback.
    if (await controller.mutate(edit.path, edit.operation, edit.value) !== null) setEditing(null);
  };
  if (frames.length === 0 && gripperFrames.length === 0 && custom.length === 0) return null;
  const parents = customTcpParentFrames(componentFrames, custom);
  const problem = editing === null ? null
    : customTcpProblem(editing.tcp, custom, componentFrames, editing.replacing);
  return (
    <section className="dcard" aria-labelledby="component-frames-title">
      <h4 id="component-frames-title">TCPs and frames</h4>
      {frames.map((target) => (
        <dl key={target.targetId} aria-label={`${target.targetId} frames`}>
          <div><dt><strong>{target.targetId}</strong></dt><dd>Applied model · read-only</dd></div>
          {target.resolvedTcp !== null && <div><dt>tcp in use</dt><dd>{target.resolvedTcp}</dd></div>}
          {target.frames.filter((frame) => !customIds.has(frame.frameRoleId)).map((frame) => {
            const tcp = runtimeTcps.find((row) => row.commandTargetId === target.targetId
              && row.frameRoleId === frame.frameRoleId);
            return (
              <div key={frame.frameRoleId}>
                <dt>{frame.frameRoleId}</dt>
                <dd>{`${frame.sourceLinkName ?? 'cell frame'}${tcp === undefined ? ''
                  : ` · TCP ${tcp.qualifiedName}${tcp.isDefault ? ' (default)' : ''}`}`}</dd>
              </div>
            );
          })}
        </dl>
      ))}
      {gripperFrames.map((gripper) => (
        <dl key={gripper.targetId} aria-label={`${gripper.targetId} gripper frames`}>
          <div><dt><strong>{gripper.targetId}</strong></dt><dd>Deployment Draft</dd></div>
          <div><dt>TCP frame</dt><dd>{gripper.tcpFrame}</dd></div>
          <div><dt>Attach frame</dt><dd>{gripper.attachFrame}</dd></div>
          <div><dt>Mount frame</dt><dd>{gripper.mountFrame}</dd></div>
        </dl>
      ))}
      <dl aria-label="Custom TCPs">
        <div><dt><strong>Custom TCPs</strong></dt><dd>Deployment Draft · {custom.length}</dd></div>
        {custom.map((tcp) => (
          <div key={tcp.tcpId}>
            <dt>{`${componentId}.${tcp.tcpId}`}</dt>
            <dd>
              {`on ${tcp.parentFrame} · [${tcp.positionM.join(', ')}] m · rpy [${tcp.rpyDeg.join(', ')}] deg `}
              <button type="button" className="text-button" disabled={disabled || noDraftRow || editing !== null}
                title={noDraftRowTitle} aria-label={`Edit custom TCP ${tcp.tcpId}`}
                onClick={() => setEditing({ replacing: tcp.tcpId, tcp: {
                  // Without `stored`: the edited row is written from the form.
                  tcpId: tcp.tcpId, parentFrame: tcp.parentFrame, positionM: [...tcp.positionM], rpyDeg: [...tcp.rpyDeg],
                } })}>Edit</button>{' '}
              <button type="button" className="text-button" disabled={disabled || noDraftRow || editing !== null}
                title={noDraftRowTitle} aria-label={`Delete custom TCP ${tcp.tcpId}`}
                onClick={() => { void save(custom.filter((row) => row.tcpId !== tcp.tcpId)); }}>Delete</button>
            </dd>
          </div>
        ))}
      </dl>
      {editing === null ? (
        <div className="dactions">
          <button type="button" disabled={disabled || noDraftRow || parents.length === 0}
            title={noDraftRowTitle ?? (parents.length === 0
              ? 'Apply the component first: a custom TCP needs one of its frames as parent.' : undefined)}
            onClick={() => setEditing({ replacing: null, tcp: {
              tcpId: '', parentFrame: parents[0] ?? '', positionM: [0, 0, 0], rpyDeg: [0, 0, 0],
            } })}>Add custom TCP</button>
        </div>
      ) : (
        <CustomTcpForm
          value={editing.tcp}
          renaming={editing.replacing === null}
          parents={parents}
          problem={problem}
          disabled={disabled}
          onChange={(tcp) => setEditing({ ...editing, tcp })}
          onCancel={() => setEditing(null)}
          onSave={() => {
            if (problem !== null) return;
            void save(editing.replacing === null
              ? [...custom, editing.tcp]
              : custom.map((row) => (row.tcpId === editing.replacing ? editing.tcp : row)));
          }}
        />
      )}
      <p className="field-help">
        Model frames and descriptor TCPs are read-only; they come from the robot or tool. A custom TCP is placed on one
        of this component's frames and runs after Apply. Model frames are the Applied ones; a Draft change shows after
        Apply. Gripper frames are edited under Current gripper configuration below.
      </p>
    </section>
  );
}

const AXES = ['x', 'y', 'z'] as const;
const ANGLES = ['roll', 'pitch', 'yaw'] as const;

function CustomTcpForm({ value, renaming, parents, problem, disabled, onChange, onCancel, onSave }: {
  value: CustomTcp;
  renaming: boolean;
  parents: readonly string[];
  problem: string | null;
  disabled: boolean;
  onChange: (tcp: CustomTcp) => void;
  onCancel: () => void;
  onSave: () => void;
}) {
  const number = (text: string) => (text.trim() === '' ? Number.NaN : Number(text));
  const problemId = useId();
  return (
    <div className="deployment-focused-body" aria-label="Custom TCP">
      <label className="deployment-focused-field">
        <span>TCP id</span>
        <input aria-label="Custom TCP id" value={value.tcpId} disabled={disabled || !renaming}
          onChange={(event) => onChange({ ...value, tcpId: event.target.value })} />
      </label>
      <label className="deployment-focused-field">
        <span>Parent frame</span>
        <select aria-label="Custom TCP parent frame" value={value.parentFrame} disabled={disabled}
          onChange={(event) => onChange({ ...value, parentFrame: event.target.value })}>
          {parents.map((frame) => <option key={frame} value={frame}>{frame}</option>)}
        </select>
      </label>
      {AXES.map((axis, index) => (
        <label key={axis} className="deployment-focused-field">
          <span>{axis} (m)</span>
          <input aria-label={`Custom TCP ${axis} (m)`} type="number" step="0.001"
            value={Number.isFinite(value.positionM[index]) ? value.positionM[index] : ''} disabled={disabled}
            onChange={(event) => {
              const positionM = [...value.positionM] as CustomTcp['positionM'];
              positionM[index] = number(event.target.value);
              onChange({ ...value, positionM });
            }} />
        </label>
      ))}
      {ANGLES.map((angle, index) => (
        <label key={angle} className="deployment-focused-field">
          <span>{angle} (deg)</span>
          <input aria-label={`Custom TCP ${angle} (deg)`} type="number" step="0.1"
            value={Number.isFinite(value.rpyDeg[index]) ? value.rpyDeg[index] : ''} disabled={disabled}
            onChange={(event) => {
              const rpyDeg = [...value.rpyDeg] as CustomTcp['rpyDeg'];
              rpyDeg[index] = number(event.target.value);
              onChange({ ...value, rpyDeg });
            }} />
        </label>
      ))}
      {/* Local validation, red; described by Save rather than announced on open. */}
      {problem !== null && <p id={problemId} className="program-form-error inline-message severity-error">{problem}</p>}
      <div className="dactions">
        <button type="button" disabled={disabled || problem !== null}
          aria-describedby={problem !== null ? problemId : undefined} onClick={onSave}>Save to Draft</button>
        <button type="button" className="quiet-button" onClick={onCancel}>Cancel</button>
      </div>
    </div>
  );
}
