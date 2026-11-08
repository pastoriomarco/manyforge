/**
 * Advisory motion preview of a failed node's Draft edits (V13.7, D4).
 *
 * While a run is held on a failed node, the operator edits the Draft with
 * the ordinary editors. When the Draft gives that node different inputs than
 * the run's captured Program, the viewer shows the effect through the
 * node-correction preview query: the node is resolved as a fresh attempt
 * would and the changed inputs are replaced. Only exact replacements are
 * previewed: an edit beneath a value source, a changed source, or a
 * parameter reached through a formula is reported unavailable, never
 * approximated. The preview never gates Apply, Reset or anything else.
 */
import type {
  CatalogEntry,
  JsonValue,
  ProgramAuthoringDocument,
  ProgramBindingDocument,
  ProgramDocument,
  ProgramFormulaReferences,
  ProgramNodeCorrectionPreview,
  ProgramTreeNode,
  ProgramValueSource,
  ProposedNodeInput,
} from '../types/api';
import type {
  DirectControlMotionStatus,
  ManualControlMotionDescriptor,
  MotionPreviewReceipt,
} from '../types/robotControl';
import {
  formatValuePath,
  parseValuePath,
  valuePathWithin,
  type ValuePathSegment,
} from '../utils/valuePaths';

/** The most proposed inputs one preview request may carry. */
const MAX_PREVIEW_INPUTS = 64;

function sourceLabel(source: ProgramValueSource): string {
  if (source.kind === 'expression') return `formula ${source.expression}`;
  if (source.kind === 'blackboard_field') {
    return `field ${source.field} of runtime key ${source.reference}`;
  }
  return source.kind === 'program_parameter'
    ? `Program parameter ${source.reference}`
    : `runtime key ${source.reference}`;
}

function topLevel(path: string): string {
  return path.split(/[.[]/, 1)[0];
}

export function findProgramNode(
  roots: readonly ProgramTreeNode[],
  instanceId: string,
): ProgramTreeNode | null {
  const pending = [...roots];
  while (pending.length > 0) {
    const current = pending.pop()!;
    if (current.instance_id === instanceId) return current;
    pending.push(...current.children);
  }
  return null;
}

/**
 * `name` and every computed key whose formula reads it, directly or through
 * another computed key: the engine's own dependency metadata, not a parser.
 */
function readersOf(
  name: string,
  computedKeys: Readonly<Record<string, string[]>>,
): Set<string> {
  const names = new Set([name]);
  for (let grew = true; grew;) {
    grew = false;
    for (const [key, references] of Object.entries(computedKeys)) {
      const keyName = `key.${key}`;
      if (!names.has(keyName) && references.some((reference) => names.has(reference))) {
        names.add(keyName);
        grew = true;
      }
    }
  }
  return names;
}

function isJsonObject(value: JsonValue | undefined): value is Record<string, JsonValue> {
  return value !== null && value !== undefined && typeof value === 'object' && !Array.isArray(value);
}

const same = (left: unknown, right: unknown) => JSON.stringify(left) === JSON.stringify(right);

/**
 * The smallest values at which `after` differs from `before`, as canonical
 * value paths. A changed shape (different keys or list length) is one change
 * of the whole value at that path.
 */
function changedValues(
  segments: ValuePathSegment[],
  before: JsonValue | undefined,
  after: JsonValue,
): ProposedNodeInput[] {
  if (same(before, after)) return [];
  const whole = [{ path: formatValuePath(segments), value: after }];
  if (isJsonObject(before) && isJsonObject(after)) {
    const keys = Object.keys(after);
    if (
      keys.length !== Object.keys(before).length
      || keys.some((key) => !Object.prototype.hasOwnProperty.call(before, key)
        || parseValuePath(formatValuePath([...segments, key])) === null)
    ) return whole;
    return keys.flatMap((key) => changedValues([...segments, key], before[key], after[key]));
  }
  if (Array.isArray(before) && Array.isArray(after) && before.length === after.length) {
    return after.flatMap((item, index) => changedValues([...segments, index], before[index], item));
  }
  return whole;
}

/** What the Draft changes for the held node's next attempt. */
export type HeldNodePreviewInputs =
  | { kind: 'unchanged' }
  | { kind: 'inputs'; inputs: ProposedNodeInput[] }
  | { kind: 'unavailable'; reason: string };

/**
 * The held node's inputs that the current Draft changes against the run's
 * captured Program, as exact replacements for the preview query.
 */
export function heldNodePreviewInputs(
  captured: ProgramDocument,
  draft: ProgramAuthoringDocument,
  nodeInstanceId: string,
  entry: CatalogEntry | null,
  formulas: ProgramFormulaReferences,
): HeldNodePreviewInputs {
  const before = findProgramNode([captured.tree], nodeInstanceId);
  if (before === null) return { kind: 'unchanged' };
  const after = findProgramNode(draft.forest, nodeInstanceId);
  if (after === null) {
    return { kind: 'unavailable', reason: 'The Draft no longer has this node.' };
  }
  if (after.node_id !== before.node_id) {
    return { kind: 'unavailable', reason: 'The Draft replaces this node with a different node type.' };
  }
  const sources = before.value_sources ?? {};
  if (!same(sources, after.value_sources ?? {})) {
    return {
      kind: 'unavailable',
      reason: 'The Draft changes where this node’s inputs come from; only changed values can be previewed.',
    };
  }
  const defaults = new Map((entry?.parameters ?? []).map(
    (parameter) => [parameter.id, parameter.default as JsonValue | undefined],
  ));
  const effective = (node: ProgramTreeNode, id: string): JsonValue | undefined => (
    Object.prototype.hasOwnProperty.call(node.parameters, id)
      ? node.parameters[id] : defaults.get(id)
  );
  const inputs: ProposedNodeInput[] = [];
  const fields = new Set([...Object.keys(before.parameters), ...Object.keys(after.parameters)]);
  for (const id of [...fields].sort()) {
    // A whole field read from a source ignores its literal: nothing changes.
    if (sources[id] !== undefined) continue;
    const value = effective(after, id);
    const previous = effective(before, id);
    if (same(previous, value)) continue;
    if (value === undefined) {
      return { kind: 'unavailable', reason: `${id} has no value in the Draft.` };
    }
    const sourced = Object.entries(sources)
      .filter(([path]) => path !== id && topLevel(path) === id);
    const edits = sourced.length === 0
      ? [{ path: id, value }]
      : changedValues([id], previous, value);
    for (const edit of edits) {
      const shadow = sourced.find(([path]) => (
        valuePathWithin(path, edit.path) || valuePathWithin(edit.path, path)
      ));
      if (shadow !== undefined) {
        return {
          kind: 'unavailable',
          reason: `${shadow[0]} comes from ${sourceLabel(shadow[1])}, so its literal value is not used.`,
        };
      }
    }
    inputs.push(...edits);
  }
  // A changed Program parameter replaces every input it feeds directly.
  const draftParameters = new Map((draft.parameters ?? []).map((row) => [row.parameter_id, row]));
  const nodeFormulas = Object.values(formulas.nodes[nodeInstanceId] ?? {});
  for (const parameter of captured.parameters ?? []) {
    const changed = draftParameters.get(parameter.parameter_id);
    if (changed !== undefined && same(changed.default, parameter.default)) continue;
    const name = `param.${parameter.parameter_id}`;
    const readers = readersOf(name, formulas.computedKeys);
    const viaFormula = nodeFormulas.some((references) => references.some((reference) => readers.has(reference)))
      || Object.values(sources).some((source) => (
        (source.kind === 'blackboard_key' || source.kind === 'blackboard_field')
        && readers.has(`key.${source.reference}`)
      ));
    const viaPaths = Object.entries(sources)
      .filter(([, source]) => source.kind === 'program_parameter' && source.reference === parameter.parameter_id)
      .map(([path]) => path)
      .sort();
    if (!viaFormula && viaPaths.length === 0) continue;
    if (changed === undefined) {
      return { kind: 'unavailable', reason: `The Draft removes Program parameter ${parameter.parameter_id}.` };
    }
    if (viaFormula) {
      return {
        kind: 'unavailable',
        reason: `${parameter.parameter_id} reaches this node through a formula or computed key, which the preview cannot re-evaluate.`,
      };
    }
    inputs.push(...viaPaths.map((path) => ({ path, value: changed.default })));
  }
  if (inputs.length === 0) return { kind: 'unchanged' };
  if (inputs.length > MAX_PREVIEW_INPUTS) {
    return { kind: 'unavailable', reason: `The Draft changes more than ${MAX_PREVIEW_INPUTS} values of this node.` };
  }
  return { kind: 'inputs', inputs };
}

/** What the preview ghost shows: the receipt in the robot model it moves. */
export interface HeldNodeGhostSource {
  receipt: MotionPreviewReceipt;
  descriptor: ManualControlMotionDescriptor;
}

/**
 * Resolve a successful preview to the robot model it moves.
 *
 * A Program move is planned through the run's captured Program binding, not
 * the manual-control binding, so the receipt is resolved through the Applied
 * Program binding: the preview's exact component and command target must match there, and that physical
 * target's model descriptor must list the receipt's joints. Null when there
 * is no successful preview; otherwise a ghost or the reason there is none.
 */
export function heldNodeGhostSource(
  result: ProgramNodeCorrectionPreview | null,
  binding: ProgramBindingDocument | null,
  motionTargets: readonly DirectControlMotionStatus[],
): { ghost: HeldNodeGhostSource } | { reason: string } | null {
  if (result === null || result.preview?.ok !== true) return null;
  const { receipt } = result.preview;
  const target = result.target;
  if (target === null) return { reason: 'The preview named no motion target.' };
  if (
    binding === null
    || binding.program_binding_id !== result.programBindingId
    || receipt.programBindingId !== result.programBindingId
    || binding.program_revision !== result.programRevision
    || receipt.programRevision !== result.programRevision
    || binding.retained_bundle_id !== receipt.retainedBundleId
    || binding.graph_revision !== receipt.graphRevision
  ) {
    return {
      reason: 'The preview was planned for a Program binding other than the Applied one.',
    };
  }
  if (
    receipt.target.componentId !== target.componentId
    || receipt.target.commandTargetId !== target.commandTargetId
    || !binding.target_bindings.some((row) => (
      row.target.component_id === target.componentId
      && row.target.command_target_id === target.commandTargetId
    ))
  ) {
    return {
      reason: `The Applied binding does not bind ${target.componentId}/${target.commandTargetId} to the previewed robot target.`,
    };
  }
  const descriptor = motionTargets.find((candidate) => (
    candidate.targetDescriptor.controllableComponentId === target.componentId
    && candidate.targetDescriptor.commandTargetId === target.commandTargetId
  ))?.targetDescriptor ?? null;
  if (descriptor === null) {
    return {
      reason: `No robot model is available for ${target.componentId}/${target.commandTargetId}.`,
    };
  }
  if (
    descriptor.joints.length !== receipt.sourceJointNames.length
    || descriptor.joints.some((joint, index) => joint.sourceJointName !== receipt.sourceJointNames[index])
  ) {
    return { reason: 'The previewed joints do not match the robot model.' };
  }
  return { ghost: { receipt, descriptor } };
}
