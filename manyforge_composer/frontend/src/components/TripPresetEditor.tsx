import { useEffect, useState } from 'react';

import type { JsonValue, ProgramNodeRecoveryPolicy, ProgramTreeNode } from '../types/api';
import { requestOpenGlobalRules } from '../state/globalRulesNavigation';
import { INTERRUPT_NODE_IDS } from '../utils/interruptAuthoring';
import { hasSourcesWithin, valuePathWithin } from '../utils/valuePaths';
import { DraftTextInput } from './common/DraftInputs';
import {
  GRASP_LOST_KEY_SCHEMA,
  graspLostPolicyValue,
  writableBoolKeyChoices,
} from './GraspLostPolicyEditor';
import { MAX_RETRY_LIMIT } from './ProgramPolicyEditor';
import {
  ProgramSchemaValueEditor,
  type ProgramSchemaValueEditorContext,
} from './property/ProgramSchemaValueEditor';

const ATTACH_NODE_ID = 'scene.attach_carried_object';

/**
 * One choice of "When the signal trips". Each writes existing node fields
 * only (docs/authoring/INTERRUPTS.md, "Choosing what a trip does"):
 * - `stop_and_raise_alarm`: Attach object's default `on_grasp_lost`;
 * - `keep_moving_set_key`: Attach object `on_grasp_lost` without Stop or
 *   alarm, setting a key;
 * - `stop_and_succeed`: `on_signal: success`;
 * - `stop_and_fail`: `on_signal: failure`, no intervention;
 * - `stop_and_ask_operator`: `on_signal: failure`, intervention allowed.
 * Stopping the whole cell is a global rule, not a node choice.
 */
export type TripPreset =
  | 'stop_and_raise_alarm'
  | 'keep_moving_set_key'
  | 'stop_and_succeed'
  | 'stop_and_fail'
  | 'stop_and_ask_operator';

export const TRIP_PRESET_LABELS: Record<TripPreset, string> = {
  stop_and_raise_alarm: 'Stop and raise the alarm (default)',
  keep_moving_set_key: 'Keep moving, set a signal',
  stop_and_succeed: 'Stop and succeed',
  stop_and_fail: 'Stop and fail (run the Fallback)',
  stop_and_ask_operator: 'Stop and ask the operator (Retry / Continue / Reset)',
};

const TRIP_PRESET_HELP: Record<TripPreset, string> = {
  stop_and_raise_alarm: "The carrying robot's motion stops, the scene.carried_object_lost alarm is raised and the object is detached at its observed pose.",
  keep_moving_set_key: 'Motion continues, with no Stop and no alarm. On loss the Runner writes true to the key once, for the tree or a global rule to catch; reset it with a Set value node before the next carry. Detach is unchanged.',
  stop_and_succeed: 'The move stops, then the node succeeds.',
  stop_and_fail: "The move stops, then the node fails and the tree's Fallback runs at once. The Run does not wait for the operator.",
  stop_and_ask_operator: "The move stops and the Run holds for the operator: Retry from the stopped state (up to the retry limit), Continue (the node fails, then the Fallback runs) or the Program's Reset.",
};

const STOP_STAGE_NOTE = 'A Stop outcome holds when the Stop held at its first stage. An escalated Stop, a lost source or a Stop fault keeps the operation held for the operator.';

const RETRY_LIMIT_MESSAGE = `A retry limit is a whole number from 1 to ${MAX_RETRY_LIMIT}. Choose Unlimited for no limit.`;

/** The presets this node offers; none when it has no trip settings. */
export function tripPresetsFor(nodeId: string): TripPreset[] {
  if (INTERRUPT_NODE_IDS.has(nodeId)) {
    return ['stop_and_succeed', 'stop_and_fail', 'stop_and_ask_operator'];
  }
  return nodeId === ATTACH_NODE_ID ? ['stop_and_raise_alarm', 'keep_moving_set_key'] : [];
}

/** The preset the node's current fields match, or `custom`. */
export function currentTripPreset(node: ProgramTreeNode): TripPreset | 'custom' {
  if (node.node_id === ATTACH_NODE_ID) {
    if (hasSourcesWithin(node.value_sources, 'on_grasp_lost')) return 'custom';
    const policy = graspLostPolicyValue(node.parameters.on_grasp_lost);
    if (policy.stop_motion && policy.raise_alarm && policy.set_key === null && policy.detach) {
      return 'stop_and_raise_alarm';
    }
    return !policy.stop_motion && !policy.raise_alarm && policy.set_key !== null
      ? 'keep_moving_set_key' : 'custom';
  }
  if (!INTERRUPT_NODE_IDS.has(node.node_id)
      || hasSourcesWithin(node.value_sources, 'on_signal')) return 'custom';
  // An omitted on_signal is success (INTERRUPTS.md section 3).
  const onSignal = node.parameters.on_signal ?? 'success';
  if (onSignal === 'success') return 'stop_and_succeed';
  if (onSignal !== 'failure') return 'custom';
  return node.recovery_policy?.allow_intervention === true
    ? 'stop_and_ask_operator' : 'stop_and_fail';
}

/** The node with one parameter set to a literal; sources inside it are dropped. */
function withLiteral(node: ProgramTreeNode, parameterId: string, value: JsonValue): ProgramTreeNode {
  const next: ProgramTreeNode = { ...node, parameters: { ...node.parameters, [parameterId]: value } };
  const sources = Object.fromEntries(Object.entries(node.value_sources ?? {})
    .filter(([path]) => !valuePathWithin(path, parameterId)));
  delete next.value_sources;
  if (Object.keys(sources).length > 0) next.value_sources = sources;
  return next;
}

/**
 * The node with this recovery policy, in the form the Node policy editor
 * authors: the stop scope only while intervention is allowed, and the
 * default policy omitted.
 */
function withRecoveryPolicy(
  node: ProgramTreeNode,
  policy: ProgramNodeRecoveryPolicy,
): ProgramTreeNode {
  const next: ProgramTreeNode = { ...node };
  delete next.recovery_policy;
  const authored: ProgramNodeRecoveryPolicy = {
    allow_intervention: policy.allow_intervention,
    max_retries: policy.max_retries,
  };
  if (policy.allow_intervention && policy.intervention_stop_scope === 'own_groups') {
    authored.intervention_stop_scope = 'own_groups';
  }
  if (authored.allow_intervention || authored.max_retries !== 0) next.recovery_policy = authored;
  return next;
}

function recoveryPolicyOf(node: ProgramTreeNode): ProgramNodeRecoveryPolicy {
  return node.recovery_policy ?? { allow_intervention: false, max_retries: 0 };
}

/** Attach object's default: Stop, alarm and detach, no key. */
const GRASP_LOST_DEFAULT = { raise_alarm: true, stop_motion: true, set_key: null, detach: true };

/** Keep moving: no Stop and no alarm, the key set, detach kept. */
function keepMovingPolicy(current: JsonValue | undefined, setKey: string | null): JsonValue {
  const policy = graspLostPolicyValue(current);
  return {
    raise_alarm: false,
    stop_motion: false,
    set_key: setKey ?? policy.set_key,
    detach: policy.detach,
  };
}

/**
 * The node with the preset's fields written and every other field kept.
 * Keep moving writes `setKey`, or keeps the current key when none is given.
 */
export function tripPresetNode(
  node: ProgramTreeNode,
  preset: TripPreset,
  setKey: string | null = null,
): ProgramTreeNode {
  if (preset === 'stop_and_raise_alarm') {
    return withLiteral(node, 'on_grasp_lost', { ...GRASP_LOST_DEFAULT });
  }
  if (preset === 'keep_moving_set_key') {
    return withLiteral(node, 'on_grasp_lost', keepMovingPolicy(node.parameters.on_grasp_lost, setKey));
  }
  const next = withLiteral(node, 'on_signal', preset === 'stop_and_succeed' ? 'success' : 'failure');
  if (preset === 'stop_and_succeed') return next;
  return withRecoveryPolicy(next, {
    ...recoveryPolicyOf(node),
    allow_intervention: preset === 'stop_and_ask_operator',
  });
}

/** The node with its retry limit changed (0 is Unlimited). */
export function tripRetryLimitNode(node: ProgramTreeNode, maxRetries: number): ProgramTreeNode {
  return withRecoveryPolicy(node, { ...recoveryPolicyOf(node), max_retries: maxRetries });
}

function retryLimitProblem(text: string): string | null {
  const value = Number(text);
  return text.trim() === '' || !Number.isInteger(value) || value < 1 || value > MAX_RETRY_LIMIT
    ? RETRY_LIMIT_MESSAGE : null;
}

/** The node's `recovery_policy.max_retries`, shown with "Stop and ask the operator". */
function TripRetryLimit({
  maxRetries,
  disabled,
  onCommit,
}: {
  maxRetries: number;
  disabled: boolean;
  onCommit: (maxRetries: number) => void;
}) {
  const [limited, setLimited] = useState(maxRetries !== 0);
  useEffect(() => setLimited(maxRetries !== 0), [maxRetries]);
  return (
    <>
      <label className="program-policy-field">
        Retry limit
        <select
          aria-label="Trip retry limit"
          value={limited ? 'limited' : 'unlimited'}
          disabled={disabled}
          onChange={(event) => {
            const nextLimited = event.target.value === 'limited';
            setLimited(nextLimited);
            if (!nextLimited && maxRetries !== 0) onCommit(0);
          }}
        >
          <option value="unlimited">Unlimited</option>
          <option value="limited">Limit to</option>
        </select>
      </label>
      {limited && (
        <label className="program-policy-field">
          Retries per run
          <DraftTextInput
            aria-label="Trip retries per run"
            type="number"
            min={1}
            max={MAX_RETRY_LIMIT}
            step={1}
            value={maxRetries === 0 ? '' : maxRetries}
            disabled={disabled}
            validate={retryLimitProblem}
            onCommit={(text) => onCommit(Number(text))}
          />
        </label>
      )}
    </>
  );
}

/**
 * "When the signal trips": one dropdown over the node's existing trip
 * fields. Each choice is one whole-node replacement (one Draft edit).
 */
export function TripPresetEditor({
  node,
  keyContext,
  disabled,
  invalidEdit = false,
  onReplace,
}: {
  node: ProgramTreeNode;
  /** The key picker's context (Attach object `on_grasp_lost`). */
  keyContext: ProgramSchemaValueEditorContext;
  disabled: boolean;
  /** An invalid Program field edit blocks leaving the Program, as the Deployment button does. */
  invalidEdit?: boolean;
  onReplace: (replacement: ProgramTreeNode) => void;
}) {
  const presets = tripPresetsFor(node.node_id);
  const current = currentTripPreset(node);
  // Keep moving needs a key: nothing is written until one is chosen.
  const [pendingKey, setPendingKey] = useState(false);
  useEffect(() => setPendingKey(false), [node.instance_id, current]);
  if (presets.length === 0) return null;

  const shown = pendingKey ? 'keep_moving_set_key' : current;
  const { keys, choices } = writableBoolKeyChoices(keyContext);
  const graspLost = graspLostPolicyValue(node.parameters.on_grasp_lost);
  const choose = (preset: TripPreset) => {
    if (preset === 'keep_moving_set_key'
        && (graspLost.set_key === null || !keys.has(graspLost.set_key))) {
      setPendingKey(true);
      return;
    }
    setPendingKey(false);
    onReplace(tripPresetNode(node, preset));
  };

  return (
    <section className="dcard" aria-label="Trip outcome">
      <h4>Trip outcome</h4>
      <label className="program-policy-field">
        When the signal trips
        <select
          aria-label="When the signal trips"
          value={shown}
          disabled={disabled}
          onChange={(event) => choose(event.target.value as TripPreset)}
        >
          {shown === 'custom' && <option value="custom" disabled>Custom</option>}
          {presets.map((preset) => (
            <option key={preset} value={preset}>{TRIP_PRESET_LABELS[preset]}</option>
          ))}
        </select>
      </label>
      <p className="program-form-hint">
        {shown !== 'custom' ? TRIP_PRESET_HELP[shown]
          : node.node_id === ATTACH_NODE_ID
            ? 'Custom: the On grasp lost settings below match no preset.'
            : 'Custom: on_signal matches no preset.'}
      </p>
      {shown === 'keep_moving_set_key' && (
        <label className="program-grasp-lost-key-field">
          Key to set on loss
          <ProgramSchemaValueEditor
            schema={GRASP_LOST_KEY_SCHEMA}
            value={graspLost.set_key ?? undefined}
            disabled={disabled}
            context={{
              ...keyContext,
              semanticChoices: choices,
              // A key created here is written with the Keep moving fields.
              onCreateReference: keyContext.onCreateReference === undefined ? undefined
                : (target, expectedType, expectedAccess, source) => keyContext.onCreateReference?.(
                  target, expectedType, expectedAccess,
                  { ...source, parameterValue: keepMovingPolicy },
                ),
            }}
            path="trip_key"
            onCommit={(next) => {
              if (typeof next !== 'string' || !keys.has(next)) return;
              setPendingKey(false);
              onReplace(tripPresetNode(node, 'keep_moving_set_key', next));
            }}
          />
        </label>
      )}
      {shown === 'keep_moving_set_key' && keys.size === 0 && keyContext.onCreateReference === undefined && (
        <p className="program-form-hint" role="note">Declare a writable bool key first.</p>
      )}
      {shown === 'stop_and_ask_operator' && (
        <TripRetryLimit
          maxRetries={recoveryPolicyOf(node).max_retries}
          disabled={disabled}
          onCommit={(maxRetries) => onReplace(tripRetryLimitNode(node, maxRetries))}
        />
      )}
      {INTERRUPT_NODE_IDS.has(node.node_id) && (
        <p className="program-form-hint">{STOP_STAGE_NOTE}</p>
      )}
      <p className="program-form-hint">
        To stop the whole cell, use a global rule with scope everything: a Full Stop the operator recovers.
        {' '}
        <button
          type="button"
          className="text-button"
          disabled={invalidEdit}
          title={invalidEdit ? 'Review or discard the invalid Program field edit first.' : undefined}
          onClick={requestOpenGlobalRules}
        >
          Open global rules
        </button>
      </p>
    </section>
  );
}
