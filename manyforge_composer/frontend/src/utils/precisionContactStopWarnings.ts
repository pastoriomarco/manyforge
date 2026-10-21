import type { JsonValue, ProgramTreeNode } from '../types/api';
import { FALLBACK_STOP_MODES_FIELD, STOP_MODE_ORDER } from './stopFallbackSequence';

/**
 * Precision-contact nodes. The catalog has no category marker for these; this
 * mirrors the Deployment compiler, which assigns `precision_contact` to
 * `motion.probe_search` only (program_io_condition_compiler.cpp).
 */
export const PRECISION_CONTACT_NODE_IDS: ReadonlySet<string> = new Set(['motion.probe_search']);

/** The arm's nominal controlled Stop as the Deployment Draft states it. */
export interface DeploymentArmControlledStop {
  stopMode: string;
  allowNaturalCompletion: boolean;
  /** Ordered fallbacks after `stopMode` (hard deceleration always follows). */
  fallbackStopModes: string[];
}

interface TargetRoleBinding {
  target_role_id: string;
  controllable_component_id: string;
  command_target_id: string;
}

type JsonObject = Record<string, JsonValue>;
const object = (value: unknown): JsonObject | null => (
  value !== null && typeof value === 'object' && !Array.isArray(value) ? value as JsonObject : null
);

function stopFromParameters(parameters: JsonObject | null): DeploymentArmControlledStop | null {
  const mode = parameters?.stop_mode;
  const natural = parameters?.allow_natural_completion_on_short_path;
  if (typeof mode !== 'string' || typeof natural !== 'boolean') return null;
  // Guarded by presence: the list field (intent v2, or a compiled document
  // that carries it) and/or a compiled v1 legacy substitution (soft only).
  const list = parameters?.[FALLBACK_STOP_MODES_FIELD];
  const legacy = parameters?.recoverable_substitution_mode;
  const recorded = [
    ...(Array.isArray(list) ? list.filter((entry): entry is string => typeof entry === 'string') : []),
    ...(mode === 'soft_on_path' && typeof legacy === 'string' ? [legacy] : []),
  ];
  return {
    stopMode: mode,
    allowNaturalCompletion: natural,
    fallbackStopModes: inFixedOrder(recorded),
  };
}

function inFixedOrder(modes: readonly string[]): string[] {
  return STOP_MODE_ORDER.filter((mode) => modes.includes(mode));
}

/**
 * One arm target's controlled Stop from the Deployment Draft: the intent's
 * `target_settings.<target>.controlled_stop`, or a compiled document's
 * `motion_controlled_stop` stage defaults (any row allowing a setting counts).
 */
export function deploymentArmControlledStop(
  document: JsonObject | null | undefined,
  componentId: string,
  targetId: string,
): DeploymentArmControlledStop | null {
  if (!document) return null;
  if (document.api_version === 'manyforge.deployment_intent.v2') {
    const component = (Array.isArray(document.components) ? document.components : [])
      .map(object).find((row) => row?.component_id === componentId);
    return stopFromParameters(object(object(object(component?.target_settings)?.[targetId])?.controlled_stop));
  }
  const graph = object(document.graph);
  const bindingIds = new Set((Array.isArray(graph?.control_bindings) ? graph.control_bindings : [])
    .map(object)
    .filter((row) => row?.controllable_component_id === componentId && row.command_target_id === targetId)
    .map((row) => row?.control_binding_id));
  const rows = object(document.policies)?.stop_stage_parameters;
  const stops = (Array.isArray(rows) ? rows : []).map(object)
    .filter((row) => row !== null && bindingIds.has(row.control_binding_id)
      && row.stop_escalation_stage_id === 'motion_controlled_stop'
      && object(row.provider_contract_ref)?.provider_contract_role_id === 'stop_contract')
    .map((row) => stopFromParameters(object(row?.default_contract_parameters)))
    .filter((stop): stop is DeploymentArmControlledStop => stop !== null);
  if (stops.length === 0) return null;
  return {
    stopMode: stops[0].stopMode,
    allowNaturalCompletion: stops.some((stop) => stop.allowNaturalCompletion),
    fallbackStopModes: inFixedOrder(stops.flatMap((stop) => stop.fallbackStopModes)),
  };
}

/**
 * Warnings (never errors) when a precision-contact node's effective Stop goes
 * against the usual probe semantics. Core builds the node's Stop from the
 * Deployment's nominal controlled Stop and refuses a node `stop_mode` that
 * differs from it (io_rule_stop_response.cpp), so the effective mode is the
 * Deployment's; natural completion and the fallback sequence also come from it.
 * Natural completion is used by every controlled mode, not by
 * `hard_deceleration`; a spring-back fallback matters to a probe only after a
 * `soft_on_path` primary (participant_stop_stage_helper.cpp).
 */
export function precisionContactStopWarnings(
  node: ProgramTreeNode,
  deploymentDocument: JsonObject | null | undefined,
  targetRoleBindings: readonly TargetRoleBinding[] | null | undefined,
): string[] {
  if (!PRECISION_CONTACT_NODE_IDS.has(node.node_id)) return [];
  const entry = node.parameters.stop_entry_stage_id;
  // A later entry stage skips the controlled Stop and its settings. (Later
  // controlled_fallback motion_controlled_stop stages would carry their own
  // settings, but none are generated today.)
  if (typeof entry === 'string' && entry !== 'motion_controlled_stop') return [];
  const binding = targetRoleBindings?.find((row) => row.target_role_id === node.parameters.target_role);
  if (binding === undefined) return [];
  const stop = deploymentArmControlledStop(
    deploymentDocument, binding.controllable_component_id, binding.command_target_id,
  );
  if (stop === null) return [];
  const mode = stop.stopMode;
  const target = `${binding.controllable_component_id} · ${binding.command_target_id}`;
  const inherited = node.value_sources?.stop_mode === undefined
    ? `is inherited from the Deployment's arm Stop settings (${target}) and applies to this node's Stop mode ${mode}.`
    : `is inherited from the Deployment's arm Stop settings (${target}). This node's Stop mode is set at run time; it must resolve to ${mode}, the Deployment's Stop mode, for the probe to run.`;
  const warnings: string[] = [];
  if (stop.allowNaturalCompletion && mode !== 'hard_deceleration') {
    warnings.push(
      'This probe may finish a short remaining path instead of stopping on contact. '
      + `Natural completion ${inherited} `
      + 'To change it, clear "Allow natural completion on a short remaining path" in the Deployment\'s Controlled Stop for this arm.',
    );
  }
  const springBack = stop.fallbackStopModes
    .filter((fallback) => fallback === 'spring_back_hold' || fallback === 'spring_back_fast');
  if (springBack.length > 0 && mode === 'soft_on_path') {
    warnings.push(
      'If the soft Stop cannot be carried out, this probe will spring back instead of stopping at the contact point. '
      + `The spring-back fallback (${springBack.join(', ')}) ${inherited} `
      + "To change it, disable the spring-back fallbacks in the Deployment's Stop settings for this arm.",
    );
  }
  return warnings;
}
