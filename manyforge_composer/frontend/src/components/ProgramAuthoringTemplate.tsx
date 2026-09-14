import { useMemo, useRef, useState } from 'react';

import type { JsonValue, ProgramAuthoringDocument } from '../types/api';
import { OverlayPanel } from './OverlayPanel';

const PORTABLE_ID = /^[a-z][a-z0-9_.-]*$/;

interface TargetChoice {
  id: string;
  frames: string[];
  collisionBodies: string[];
}

function record(value: JsonValue): Record<string, JsonValue> | null {
  return value !== null && typeof value === 'object' && !Array.isArray(value)
    ? value as Record<string, JsonValue>
    : null;
}

function strings(value: JsonValue | undefined): string[] {
  return Array.isArray(value)
    ? value.filter((item): item is string => typeof item === 'string')
    : [];
}

function hasCapability(value: JsonValue, capabilityId: string): boolean {
  const target = record(value);
  const capabilities = target?.required_capabilities;
  return Array.isArray(capabilities) && capabilities.some((item) => (
    record(item)?.capability_id === capabilityId
  ));
}

function targets(
  program: ProgramAuthoringDocument,
  capabilityId: string,
): TargetChoice[] {
  return Object.entries(program.target_roles)
    .filter(([, value]) => hasCapability(value, capabilityId))
    .map(([id, value]) => {
      const target = record(value);
      return {
        id,
        frames: strings(target?.required_frame_roles),
        collisionBodies: strings(target?.required_collision_body_roles),
      };
    })
    .sort((left, right) => left.id.localeCompare(right.id));
}

function contactPolicyIds(program: ProgramAuthoringDocument): string[] {
  return (program.contact_policies ?? []).flatMap((value) => {
    const identity = record(value)?.policy_id;
    return typeof identity === 'string' ? [identity] : [];
  }).sort();
}

export function ProgramAuthoringTemplate({
  program,
  busy,
  onMutate,
}: {
  program: ProgramAuthoringDocument;
  busy: boolean;
  onMutate: (mutation: string, args: Record<string, JsonValue>) => void;
}) {
  const pickButton = useRef<HTMLButtonElement>(null);
  const placeButton = useRef<HTMLButtonElement>(null);
  const [open, setOpen] = useState(false);
  const [operation, setOperation] = useState<'pick' | 'place'>('pick');
  const armTargets = useMemo(
    () => targets(program, 'planning.motion.pose_goal'),
    [program],
  );
  const toolTargets = useMemo(
    () => targets(program, 'tool.gripper.command'),
    [program],
  );
  const scenes = useMemo(
    () => (program.scene ?? []).map((row) => row.scene_resource_id).sort(),
    [program.scene],
  );
  const policies = useMemo(() => contactPolicyIds(program), [program]);
  const [sceneId, setSceneId] = useState('');
  const [armTargetId, setArmTargetId] = useState('');
  const [toolTargetId, setToolTargetId] = useState('');
  const [baseFrame, setBaseFrame] = useState('');
  const [tcpFrame, setTcpFrame] = useState('');
  const [carrierFrame, setCarrierFrame] = useState('');
  const [touchBody, setTouchBody] = useState('');
  const [policyId, setPolicyId] = useState('');
  const [coordinateKind, setCoordinateKind] = useState<'linear_m' | 'angular_rad'>('linear_m');
  const [toolPosition, setToolPosition] = useState(0);
  const [namespaceId, setNamespaceId] = useState('planning');
  const [blackboardKey, setBlackboardKey] = useState('planning.pick_plan');
  const [instancePrefix, setInstancePrefix] = useState('pick_sequence');
  const [placePosition, setPlacePosition] = useState([0, 0, 0.1]);
  const [placeOrientation, setPlaceOrientation] = useState([0, 0, 0]);

  const arm = armTargets.find((row) => row.id === armTargetId) ?? armTargets[0];
  const tool = toolTargets.find((row) => row.id === toolTargetId) ?? toolTargets[0];
  const effectiveScene = scenes.includes(sceneId) ? sceneId : scenes[0] ?? '';
  const effectiveArm = arm?.id ?? '';
  const effectiveTool = tool?.id ?? '';
  const effectiveBase = arm?.frames.includes(baseFrame)
    ? baseFrame
    : arm?.frames.find((frame) => frame === 'base') ?? arm?.frames[0] ?? '';
  const effectiveTcp = arm?.frames.includes(tcpFrame)
    ? tcpFrame
    : arm?.frames.find((frame) => frame === 'tcp') ?? arm?.frames[0] ?? '';
  const effectiveCarrier = tool?.frames.includes(carrierFrame)
    ? carrierFrame
    : tool?.frames.find((frame) => frame === 'tcp') ?? tool?.frames[0] ?? '';
  const effectiveTouch = tool?.collisionBodies.includes(touchBody)
    ? touchBody
    : tool?.collisionBodies[0] ?? '';
  const effectivePolicy = policies.includes(policyId) ? policyId : policies[0] ?? '';
  const available = scenes.length > 0
    && arm !== undefined
    && tool !== undefined
    && effectiveBase !== ''
    && effectiveTcp !== ''
    && effectiveCarrier !== ''
    && (operation === 'place' || effectiveTouch !== '')
    && (operation === 'place' || effectivePolicy !== '')
    && PORTABLE_ID.test(namespaceId)
    && PORTABLE_ID.test(blackboardKey)
    && blackboardKey.startsWith(`${namespaceId}.`)
    && PORTABLE_ID.test(instancePrefix)
    && Number.isFinite(toolPosition)
    && placePosition.every(Number.isFinite)
    && placeOrientation.every(Number.isFinite);

  const openTemplate = (kind: 'pick' | 'place') => {
    setOperation(kind);
    setNamespaceId('planning');
    setBlackboardKey(`planning.${kind}_plan`);
    setInstancePrefix(`${kind}_sequence`);
    setOpen(true);
  };

  return (
    <section className="program-template-entry" aria-label="Composer authoring templates">
      <div className="program-template-grid">
        <button
          ref={pickButton}
          type="button"
          disabled={busy}
          onClick={() => openTemplate('pick')}
        >
          <span aria-hidden="true">◇</span>
          <span><strong>Pick subtree</strong><small>Visible ordinary nodes</small></span>
        </button>
        <button
          ref={placeButton}
          type="button"
          disabled={busy}
          onClick={() => openTemplate('place')}
        >
          <span aria-hidden="true">◇</span>
          <span><strong>Place subtree</strong><small>Visible ordinary nodes</small></span>
        </button>
      </div>
      <OverlayPanel
        open={open}
        title={`Insert ${operation} subtree`}
        titleId="pick-plan-template-title"
        onClose={() => setOpen(false)}
        returnFocusRef={operation === 'pick' ? pickButton : placeButton}
      >
        <form
          className="program-template-form"
          onSubmit={(event) => {
            event.preventDefault();
            if (!available || busy) return;
            onMutate('program_draft_expand_authoring_macro', {
              macro_id: `${operation}_subtree`,
              scene_resource_id: effectiveScene,
              target_role: effectiveArm,
              tool_target_role: effectiveTool,
              base_frame_role: effectiveBase,
              tcp_frame_role: effectiveTcp,
              carrier_frame_role: effectiveCarrier,
              tool_position: coordinateKind === 'linear_m'
                ? { kind: coordinateKind, value_m: toolPosition }
                : { kind: coordinateKind, value_rad: toolPosition },
              ...(operation === 'pick' ? {
                contact_policy_id: effectivePolicy,
                touch_body_roles: [effectiveTouch],
              } : {
                place_pose: {
                  frame_ref: {
                    kind: 'target_frame',
                    target_role: effectiveArm,
                    frame_role: effectiveBase,
                  },
                  position_m: placePosition,
                  orientation: { rpy_deg: placeOrientation },
                },
                approach_offset_m: [0, 0, 0.05],
              }),
              blackboard_namespace_id: namespaceId,
              blackboard_key: blackboardKey,
              instance_prefix: instancePrefix,
            });
            setOpen(false);
          }}
        >
          <p>
            This inserts one visible Sequence made only from ordinary planning,
            motion, gripper, and scene nodes. It remains normal editable tree
            content after insertion.
          </p>
          {!available && (
            <p className="scene-commit-error" role="status">
              The template needs a Program scene resource, pose-capable arm,
              gripper target and frame/body roles. Pick also needs a contact policy.
            </p>
          )}
          <label><span>{operation === 'pick' ? 'Object to pick' : 'Carried object'}</span><select value={effectiveScene} onChange={(event) => setSceneId(event.target.value)}>{scenes.map((id) => <option key={id}>{id}</option>)}</select></label>
          <label><span>Arm target</span><select value={effectiveArm} onChange={(event) => setArmTargetId(event.target.value)}>{armTargets.map((row) => <option key={row.id}>{row.id}</option>)}</select></label>
          <label><span>Tool target</span><select value={effectiveTool} onChange={(event) => setToolTargetId(event.target.value)}>{toolTargets.map((row) => <option key={row.id}>{row.id}</option>)}</select></label>
          <div className="program-template-grid">
            <label><span>Base frame</span><select value={effectiveBase} onChange={(event) => setBaseFrame(event.target.value)}>{arm?.frames.map((id) => <option key={id}>{id}</option>)}</select></label>
            <label><span>TCP frame</span><select value={effectiveTcp} onChange={(event) => setTcpFrame(event.target.value)}>{arm?.frames.map((id) => <option key={id}>{id}</option>)}</select></label>
            <label><span>Carrier frame</span><select value={effectiveCarrier} onChange={(event) => setCarrierFrame(event.target.value)}>{tool?.frames.map((id) => <option key={id}>{id}</option>)}</select></label>
            {operation === 'pick' && <label><span>Touch body</span><select value={effectiveTouch} onChange={(event) => setTouchBody(event.target.value)}>{tool?.collisionBodies.map((id) => <option key={id}>{id}</option>)}</select></label>}
          </div>
          {operation === 'pick' && <label><span>Contact policy</span><select value={effectivePolicy} onChange={(event) => setPolicyId(event.target.value)}>{policies.map((id) => <option key={id}>{id}</option>)}</select></label>}
          {operation === 'place' && (
            <fieldset>
              <legend>Place target in arm base frame</legend>
              <div className="program-template-grid">
                {(['X (m)', 'Y (m)', 'Z (m)'] as const).map((label, index) => <label key={label}><span>{label}</span><input type="number" step="any" value={placePosition[index]} onChange={(event) => setPlacePosition((current) => current.map((value, position) => position === index ? event.currentTarget.valueAsNumber : value))} /></label>)}
                {(['Roll (deg)', 'Pitch (deg)', 'Yaw (deg)'] as const).map((label, index) => <label key={label}><span>{label}</span><input type="number" step="any" value={placeOrientation[index]} onChange={(event) => setPlaceOrientation((current) => current.map((value, position) => position === index ? event.currentTarget.valueAsNumber : value))} /></label>)}
              </div>
            </fieldset>
          )}
          <div className="program-template-grid">
            <label><span>Tool coordinate</span><select value={coordinateKind} onChange={(event) => setCoordinateKind(event.target.value as 'linear_m' | 'angular_rad')}><option value="linear_m">Linear (m)</option><option value="angular_rad">Angular (rad)</option></select></label>
            <label><span>{operation === 'pick' ? 'Closed' : 'Open'} position</span><input type="number" step="any" value={Number.isFinite(toolPosition) ? toolPosition : ''} onChange={(event) => setToolPosition(event.currentTarget.valueAsNumber)} /></label>
            <label><span>Blackboard namespace</span><input value={namespaceId} onChange={(event) => setNamespaceId(event.target.value)} /></label>
            <label><span>Result key</span><input value={blackboardKey} onChange={(event) => setBlackboardKey(event.target.value)} /></label>
          </div>
          <label><span>Instance prefix</span><input value={instancePrefix} onChange={(event) => setInstancePrefix(event.target.value)} /></label>
          <div className="overlay-actions">
            <button type="submit" disabled={busy || !available}>Insert visible template</button>
          </div>
        </form>
      </OverlayPanel>
    </section>
  );
}
