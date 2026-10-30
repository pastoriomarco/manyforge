import type { JsonValue } from '../types/api';
import type { ComponentProfileFacts } from '../types/componentCatalog';
import { draftAttachments, fromDraft } from '../state/kinematicIndex';
import { draftComponents } from './componentManager';
import { componentRowIndex, customTcpsOf } from './customTcps';

/**
 * B5 (C-P1) Deployment choices, read from the Deployment Draft and the
 * component-type facts: a component's default TCP and a mount's primary
 * tool, each needed only in its ambiguous case, and the B-run-1 warning for
 * a robot whose primary flange tool has no TCP.
 */

type Profiles = Readonly<Record<string, ComponentProfileFacts>>;

/** One TCP of a component: a frame role on one of its command targets. */
export interface TcpPair {
  commandTargetId: string;
  frameRoleId: string;
}

function record(value: JsonValue | undefined): Record<string, JsonValue> | null {
  return value !== null && value !== undefined && typeof value === 'object' && !Array.isArray(value)
    ? value : null;
}

function draftRow(document: Record<string, JsonValue> | null, componentId: string) {
  return draftComponents(document).find((row) => row.component_id === componentId);
}

function factsOf(
  document: Record<string, JsonValue> | null,
  componentId: string,
  profiles: Profiles,
): ComponentProfileFacts | undefined {
  const profile = draftRow(document, componentId)?.component_profile_id;
  return typeof profile === 'string' ? profiles[profile] : undefined;
}

/** A pair as the selectors show it, e.g. `tcp (arm)`. */
export function tcpPairLabel(pair: TcpPair): string {
  return `${pair.frameRoleId} (${pair.commandTargetId})`;
}

/**
 * The component's Default TCP choice, or null when none is needed: its type
 * has TCPs on at most one target, whose declared default is used. The
 * options are the compiler's: on each target with TCPs, its tagged TCPs and
 * the component's custom TCPs. `value` is the Draft's `default_tcp`, if any.
 */
export function defaultTcpChoice(
  document: Record<string, JsonValue> | null,
  componentId: string,
  profiles: Profiles,
): { options: TcpPair[]; value: TcpPair | null } | null {
  const targets = factsOf(document, componentId, profiles)?.tcpTargets ?? [];
  if (targets.length < 2) return null;
  const custom = customTcpsOf(document, componentId).map((tcp) => tcp.tcpId);
  const authored = record(draftRow(document, componentId)?.default_tcp);
  return {
    options: targets.flatMap((target) => [...target.tcpFrameRoleIds, ...custom].map((frameRoleId) => ({
      commandTargetId: target.commandTargetId, frameRoleId,
    }))),
    value: typeof authored?.command_target_id === 'string' && typeof authored.frame_role_id === 'string'
      ? { commandTargetId: authored.command_target_id, frameRoleId: authored.frame_role_id }
      : null,
  };
}

/** The Draft edit that sets a component's `default_tcp`; null without its intent row. */
export function defaultTcpEdit(
  document: Record<string, JsonValue> | null,
  componentId: string,
  pair: TcpPair,
): { path: Array<string | number>; operation: 'set'; value: JsonValue } | null {
  const index = componentRowIndex(document, componentId);
  return index < 0 ? null : {
    path: ['components', index, 'default_tcp'],
    operation: 'set',
    value: { command_target_id: pair.commandTargetId, frame_role_id: pair.frameRoleId },
  };
}

/** One interface of a component that carries two or more tools. */
export interface PrimaryToolChoice {
  interfaceId: string;
  tools: string[];
  /** The one tool marked primary; null when none or several are. */
  primary: string | null;
}

/**
 * The component's interfaces that carry two or more `tool`-tagged components
 * in the Draft: each needs exactly one of them marked primary.
 */
export function primaryToolChoices(
  document: Record<string, JsonValue> | null,
  componentId: string,
  profiles: Profiles,
): PrimaryToolChoice[] {
  const mounts = draftAttachments(document).filter((row) => row.parent.componentId === componentId);
  return [...new Set(mounts.map((row) => row.parent.interface))].flatMap((interfaceId) => {
    const tools = mounts.filter((row) => row.parent.interface === interfaceId
      && factsOf(document, row.child.componentId, profiles)?.tags.includes('tool'));
    const marked = tools.filter((row) => row.primary);
    return tools.length < 2 ? [] : [{
      interfaceId,
      tools: tools.map((row) => row.child.componentId),
      primary: marked.length === 1 ? marked[0].child.componentId : null,
    }];
  });
}

/** A robot whose primary flange tool has no TCP (B-run-1). */
export interface ToolWithoutTcp {
  robotId: string;
  toolId: string;
}

/**
 * The Draft's robots whose flange tool (the primary one) has no TCP: they
 * have no end-effector TCP, so Execute is refused until one is marked.
 */
export function toolsWithoutTcp(
  document: Record<string, JsonValue> | null,
  profiles: Profiles,
): ToolWithoutTcp[] {
  const index = fromDraft(document);
  return draftComponents(document).flatMap((row) => {
    const robotId = row.component_id as string;
    const toolId = index.toolOn(robotId, 'flange');
    const facts = toolId === null ? undefined : factsOf(document, toolId, profiles);
    return toolId !== null && facts?.tcpTargets.length === 0 ? [{ robotId, toolId }] : [];
  });
}

/** The non-blocking Deployment warning of one robot without a TCP. */
export function toolWithoutTcpWarning(item: ToolWithoutTcp): string {
  return `${item.toolId} on ${item.robotId} has no TCP; Programs can't run until one of its frames is marked as a TCP.`;
}
