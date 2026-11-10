import { useMemo, useRef, useState } from 'react';

import type { JsonValue, ProgramAuthoringDocument } from '../types/api';
import { programTcpSelectorChoices } from '../utils/motionEndpointAuthoring';
import type { ReferenceSelectorFacts } from '../utils/referenceValidation';
import { OverlayPanel } from './OverlayPanel';
import { ProgramTemplateIcon } from './programNodeGlyphs';

const PORTABLE_ID = /^[a-z][a-z0-9_.-]*$/;

function record(value: JsonValue): Record<string, JsonValue> | null {
  return value !== null && typeof value === 'object' && !Array.isArray(value)
    ? value as Record<string, JsonValue> : null;
}
const refKey = (value: object) => JSON.stringify(value);
const sameTarget = (a: {component_id: string; command_target_id: string}, b: {component_id: string; command_target_id: string}) =>
  a.component_id === b.component_id && a.command_target_id === b.command_target_id;

function contactPolicyIds(program: ProgramAuthoringDocument): string[] {
  return (program.contact_policies ?? []).flatMap((value) => {
    const identity = record(value)?.policy_id;
    return typeof identity === 'string' ? [identity] : [];
  }).sort();
}

export function ProgramAuthoringTemplate({
  program,
  selectorFacts = null,
  busy,
  onMutate,
  collapsed,
  onCollapsedChange,
}: {
  program: ProgramAuthoringDocument;
  selectorFacts?: ReferenceSelectorFacts | null;
  busy: boolean;
  onMutate: (mutation: string, args: Record<string, JsonValue>) => void;
  collapsed?: boolean;
  onCollapsedChange?: (collapsed: boolean) => void;
}) {
  const pickButton = useRef<HTMLButtonElement>(null);
  const placeButton = useRef<HTMLButtonElement>(null);
  const [open, setOpen] = useState(false);
  const [operation, setOperation] = useState<'pick' | 'place'>('pick');
  const armTargets = useMemo(() => (selectorFacts?.targets ?? []).map((row) => ({ ...row,
    capability: row.capabilities.find((item) => item.capability_id === 'planning.motion.pose_goal') })), [selectorFacts]);
  const toolTargets = useMemo(() => (selectorFacts?.targets ?? []).map((row) => ({ ...row,
    capability: row.capabilities.find((item) => item.capability_id === 'tool.gripper.command') })), [selectorFacts]);
  const scenes = useMemo(
    () => (program.scene ?? []).map((row) => row.scene_resource_id).sort(),
    [program.scene],
  );
  const policies = useMemo(() => contactPolicyIds(program), [program]);
  const [sceneId, setSceneId] = useState('');
  const [armTargetId, setArmTargetId] = useState('');
  const [toolTargetId, setToolTargetId] = useState('');
  const [baseFrame, setBaseFrame] = useState('');
  const [endpointKey, setEndpointKey] = useState('');
  const [showOtherRobotTcps, setShowOtherRobotTcps] = useState(false);
  const [carrierFrame, setCarrierFrame] = useState('');
  const [touchBody, setTouchBody] = useState('');
  const [policyId, setPolicyId] = useState('');
  const [coordinateKind, setCoordinateKind] = useState<'linear_m' | 'angular_rad'>('linear_m');
  const [toolPosition, setToolPosition] = useState(0);
  const [namespaceId, setNamespaceId] = useState('planning');
  const [blackboardKey, setBlackboardKey] = useState('planning.pick_plan');
  const [instancePrefix, setInstancePrefix] = useState('pick_sequence');
  const [placePosition, setPlacePosition] = useState([0, 0, 0.1]);
  const [placeOrientation, setPlaceOrientation] = useState([180, 0, 0]);

  const arm = armTargets.find((row) => refKey(row.target) === armTargetId && row.capability?.available)
    ?? armTargets.find((row) => row.capability?.available);
  const tool = toolTargets.find((row) => refKey(row.target) === toolTargetId && row.capability?.available)
    ?? toolTargets.find((row) => row.capability?.available);
  const effectiveScene = scenes.includes(sceneId) ? sceneId : scenes[0] ?? '';
  const effectiveArm = arm === undefined ? '' : refKey(arm.target);
  const effectiveTool = tool === undefined ? '' : refKey(tool.target);
  const endpointChoices = programTcpSelectorChoices(arm?.target ?? null, selectorFacts?.targets ?? null, showOtherRobotTcps);
  const effectiveEndpoint = endpointChoices.find((choice) => choice.key === endpointKey && !choice.disabled)
    ?? endpointChoices.find((choice) => !choice.disabled);
  const armFrameChoices = (selectorFacts?.frames ?? []).map((row) => ({ ...row, eligible: !row.ambiguous && arm !== undefined
    && row.moving_targets.some((target) => sameTarget(target, arm.target)) }));
  const toolFrameChoices = (selectorFacts?.frames ?? []).map((row) => ({ ...row, eligible: !row.ambiguous && tool !== undefined
    && row.moving_targets.some((target) => sameTarget(target, tool.target)) }));
  const toolBodyChoices = (selectorFacts?.bodies ?? []).map((row) => ({ ...row, eligible: !row.ambiguous && tool !== undefined
    && row.moving_targets.some((target) => sameTarget(target, tool.target)) }));
  const armFrames = armFrameChoices.filter((row) => row.eligible);
  const toolFrames = toolFrameChoices.filter((row) => row.eligible);
  const toolBodies = toolBodyChoices.filter((row) => row.eligible);
  const selectedBase = armFrames.find((row) => refKey(row.reference) === baseFrame)
    ?? armFrames.find((row) => row.reference.frame_id === 'base') ?? armFrames[0];
  const selectedCarrier = toolFrames.find((row) => refKey(row.reference) === carrierFrame)
    ?? toolFrames.find((row) => row.reference.frame_id === 'tcp') ?? toolFrames[0];
  const selectedTouch = toolBodies.find((row) => refKey(row.reference) === touchBody) ?? toolBodies[0];
  const effectiveBase = selectedBase === undefined ? '' : refKey(selectedBase.reference);
  const effectiveCarrier = selectedCarrier === undefined ? '' : refKey(selectedCarrier.reference);
  const effectiveTouch = selectedTouch === undefined ? '' : refKey(selectedTouch.reference);
  const effectivePolicy = policies.includes(policyId) ? policyId : policies[0] ?? '';
  const available = scenes.length > 0
    && arm !== undefined
    && tool !== undefined
    && effectiveBase !== ''
    && effectiveEndpoint !== undefined
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

  // Collapsed by default, like a catalog category the user has not opened. The
  // palette may control it so its collapse-all/expand-all also covers Templates.
  const [ownCollapsed, setOwnCollapsed] = useState(true);
  const templatesCollapsed = collapsed ?? ownCollapsed;
  const setTemplatesCollapsed = (next: (current: boolean) => boolean) => {
    const value = next(templatesCollapsed);
    if (onCollapsedChange) onCollapsedChange(value);
    else setOwnCollapsed(value);
  };
  const openTemplate = (kind: 'pick' | 'place') => {
    setOperation(kind);
    setEndpointKey('');
    setShowOtherRobotTcps(false);
    setNamespaceId('planning');
    setBlackboardKey(`planning.${kind}_plan`);
    setInstancePrefix(`${kind}_sequence`);
    setOpen(true);
  };

  return (
    <section className="program-template-entry" aria-label="Composer authoring templates">
      {/* The catalog's category header, so templates read as their own group. */}
      <header className="program-palette-category" data-category-family="gripper">
        <strong>Templates</strong>
        <div>
          <small>2</small>
          <button
            type="button"
            className="icon-button program-palette-category-toggle"
            aria-label={`${templatesCollapsed ? 'Expand' : 'Collapse'} Templates`}
            aria-expanded={!templatesCollapsed}
            title={`${templatesCollapsed ? 'Expand' : 'Collapse'} Templates`}
            onClick={() => setTemplatesCollapsed((collapsed) => !collapsed)}
          >
            {templatesCollapsed ? '⌃' : '⌄'}
          </button>
        </div>
      </header>
      <div className="program-template-buttons" hidden={templatesCollapsed}>
        <button
          ref={pickButton}
          type="button"
          disabled={busy}
          aria-label="Pick subtree"
          title="Pick subtree: opens a short form, then inserts ordinary nodes you can edit."
          onClick={() => openTemplate('pick')}
        >
          <ProgramTemplateIcon kind="pick" />
          Pick
        </button>
        <button
          ref={placeButton}
          type="button"
          disabled={busy}
          aria-label="Place subtree"
          title="Place subtree: opens a short form, then inserts ordinary nodes you can edit."
          onClick={() => openTemplate('place')}
        >
          <ProgramTemplateIcon kind="place" />
          Place
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
            if (!available || busy || effectiveEndpoint === undefined || arm === undefined || tool === undefined || selectedBase === undefined || selectedCarrier === undefined) return;
            onMutate('program_draft_expand_authoring_macro', {
              macro_id: `${operation}_subtree`,
              scene_resource_id: effectiveScene,
              target: arm.target,
              tool_target: tool.target,
              base_frame: selectedBase.reference,
              tcp_endpoint: effectiveEndpoint.selector,
              carrier_frame_ref: selectedCarrier.reference,
              tool_position: coordinateKind === 'linear_m'
                ? { kind: coordinateKind, value_m: toolPosition }
                : { kind: coordinateKind, value_rad: toolPosition },
              ...(operation === 'pick' ? {
                contact_policy_id: effectivePolicy,
                touch_bodies: selectedTouch === undefined ? [] : [selectedTouch.reference],
              } : {
                place_pose: {
                  frame_ref: {
                    kind: 'target_frame',
                    frame: selectedBase.reference,
                  },
                  position_m: placePosition,
                  orientation: { rpy_deg: placeOrientation },
                },
                approach_offset_m: [0, 0, 0.05],
              }),
              blackboard_namespace_id: namespaceId,
              blackboard_key: blackboardKey,
              instance_prefix: instancePrefix,
            } as unknown as Record<string, JsonValue>);
            setOpen(false);
          }}
        >
          <p>
            This inserts one visible Sequence made only from ordinary planning,
            motion, gripper, and scene nodes. It remains normal editable tree
            content after insertion.
          </p>
          {!available && (
            <p className="scene-commit-error inline-message severity-error" role="status">
              The template needs a Program scene resource, pose-capable arm,
              gripper target and qualified frames/bodies. Pick also needs a contact policy.
            </p>
          )}
          <label><span>{operation === 'pick' ? 'Object to pick' : 'Carried object'}</span><select value={effectiveScene} onChange={(event) => setSceneId(event.target.value)}>{scenes.map((id) => <option key={id}>{id}</option>)}</select></label>
          <label><span>Arm target</span><select value={effectiveArm} onChange={(event) => setArmTargetId(event.target.value)}>{armTargets.map((row) => <option key={refKey(row.target)} value={refKey(row.target)} disabled={!row.capability?.available} title={row.capability?.reason ?? undefined}>{row.target.component_id} · {row.target.command_target_id}{row.capability?.available ? '' : ` · ${row.capability?.reason ?? 'Capability unavailable'}`}</option>)}</select></label>
          <label><span>Tool target</span><select value={effectiveTool} onChange={(event) => setToolTargetId(event.target.value)}>{toolTargets.map((row) => <option key={refKey(row.target)} value={refKey(row.target)} disabled={!row.capability?.available} title={row.capability?.reason ?? undefined}>{row.target.component_id} · {row.target.command_target_id}{row.capability?.available ? '' : ` · ${row.capability?.reason ?? 'Capability unavailable'}`}</option>)}</select></label>
          <div className="program-template-grid">
            <label><span>Base frame</span><select value={effectiveBase} onChange={(event) => setBaseFrame(event.target.value)}>{armFrameChoices.map((row) => <option key={refKey(row.reference)} value={refKey(row.reference)} disabled={!row.eligible} title={!row.eligible ? 'Not qualified for the selected target' : undefined}>{row.reference.component_id} · {row.reference.frame_id}{row.eligible ? '' : ' · Not qualified for the selected target'}</option>)}</select></label>
            <p className="program-form-hint">{`Moves: ${arm ? `${arm.target.component_id} · ${arm.target.command_target_id}` : '—'}`}</p>
            <label><span>Motion TCP endpoint</span><select value={effectiveEndpoint?.key ?? ''} onChange={(event) => setEndpointKey(event.target.value)}>{endpointChoices.map((choice) => <option key={choice.key} value={choice.key} disabled={choice.disabled} title={choice.reason}>{choice.label}{choice.reason ? ` · ${choice.reason}` : ''}</option>)}</select></label>
            <label title="Useful only once tool change is implemented: today a TCP whose tool is on another robot is refused when used.">
              <input type="checkbox" checked={showOtherRobotTcps}
                onChange={(event) => setShowOtherRobotTcps(event.currentTarget.checked)} />
              Show TCPs of tools mounted on other robots
            </label>
            {selectorFacts === null && <p className="program-form-hint">Qualified Draft choices are unavailable.</p>}
            <label><span>Carrier frame</span><select value={effectiveCarrier} onChange={(event) => setCarrierFrame(event.target.value)}>{toolFrameChoices.map((row) => <option key={refKey(row.reference)} value={refKey(row.reference)} disabled={!row.eligible} title={!row.eligible ? 'Not qualified for the selected target' : undefined}>{row.reference.component_id} · {row.reference.frame_id}{row.eligible ? '' : ' · Not qualified for the selected target'}</option>)}</select></label>
            {operation === 'pick' && <label><span>Touch body</span><select value={effectiveTouch} onChange={(event) => setTouchBody(event.target.value)}>{toolBodyChoices.map((row) => <option key={refKey(row.reference)} value={refKey(row.reference)} disabled={!row.eligible} title={!row.eligible ? 'Not qualified for the selected target' : undefined}>{row.reference.component_id} · {row.reference.collision_body_id}{row.eligible ? '' : ' · Not qualified for the selected target'}</option>)}</select></label>}
          </div>
          {operation === 'pick' && <label><span>Contact policy</span><select value={effectivePolicy} onChange={(event) => setPolicyId(event.target.value)}>{policies.map((id) => <option key={id}>{id}</option>)}</select></label>}
          {operation === 'place' && (
            <fieldset>
              <legend>Gripper (TCP) pose at release, in arm base frame</legend>
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
          <div className="dialog-actions">
            <button type="submit" className="primary-action" disabled={busy || !available}>Insert visible template</button>
          </div>
        </form>
      </OverlayPanel>
    </section>
  );
}
