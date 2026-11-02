import type { JsonValue, ProgramAuthoringDocument, ProgramTreeNode } from '../types/api';

export interface MotionEndpointChoice {
  key: string;
  targetRole: string;
  frameRole: string;
  label: string;
}

function objectValue(value: JsonValue | undefined): Record<string, JsonValue> | null {
  return value !== null && value !== undefined && typeof value === 'object' && !Array.isArray(value)
    ? value as Record<string, JsonValue>
    : null;
}

export function motionEndpointChoiceKey(targetRole: string, frameRole: string): string {
  return JSON.stringify(['entry', targetRole, frameRole]);
}

/**
 * One TCP offered for a target role in the current Deployment: a TCP the
 * bound robot's Robot Control descriptor lists (owner-qualified, default
 * marked) or a custom TCP of the bound component still in the Draft.
 */
export interface AvailableMotionTcp {
  targetRole: string;
  frameRole: string;
  qualifiedName: string;
  isDefault: boolean;
  /** A custom TCP that runs only after Apply. */
  draftOnly?: boolean;
  mountedElsewhere?: boolean;
}

/**
 * The target/frame pairs the Program has declared, plus every TCP the
 * current Deployment offers for its target roles (TCP-C). Picking an
 * undeclared TCP declares it on the target role in the same Program edit.
 * `resolve` may name what a declared frame role resolves to (a robot's
 * `tcp`, D1); it is used when no owner-qualified name is available.
 */
export function programMotionEndpointChoices(
  program: Pick<ProgramAuthoringDocument, 'target_roles'>,
  resolve: (targetRole: string, frameRole: string) => string | null = () => null,
  available: readonly AvailableMotionTcp[] = [],
): MotionEndpointChoice[] {
  const choices = new Map<string, MotionEndpointChoice>();
  for (const [targetRole, rawTarget] of Object.entries(program.target_roles)) {
    const frames = objectValue(rawTarget)?.required_frame_roles;
    if (!Array.isArray(frames)) continue;
    for (const frameRole of frames) {
      if (typeof frameRole !== 'string' || frameRole === '') continue;
      const resolved = resolve(targetRole, frameRole);
      const key = motionEndpointChoiceKey(targetRole, frameRole);
      choices.set(key, {
        key, targetRole, frameRole,
        label: `${targetRole} · ${frameRole}${resolved === null ? '' : ` → ${resolved}`}`,
      });
    }
  }
  for (const tcp of available) {
    if (!(tcp.targetRole in program.target_roles)) continue;
    const key = motionEndpointChoiceKey(tcp.targetRole, tcp.frameRole);
    choices.set(key, {
      key, targetRole: tcp.targetRole, frameRole: tcp.frameRole,
      label: `${tcp.qualifiedName}${tcp.isDefault ? ' (default)' : ''}`
        + `${tcp.draftOnly ? ' (after Apply)' : ''} · ${tcp.targetRole} · ${tcp.frameRole}`,
    });
  }
  return [...choices.values()].sort((left, right) => left.label.localeCompare(right.label));
}

/**
 * The TCPs the current Deployment offers per bound Program target role: the
 * Applied TCPs of each robot's Robot Control descriptor (its own TCPs on the
 * role bound to the robot target, its mounted tool's TCPs on the role bound
 * to the tool target, e.g. `{gripper, cup_edge}`), and the Draft custom TCPs
 * of those components that are not Applied yet.
 */
export function availableMotionTcps(
  boundTargets: ReadonlyArray<{ target_role_id: string; controllable_component_id: string; command_target_id: string }>,
  descriptors: ReadonlyArray<{
    tcps: ReadonlyArray<{
      controllableComponentId: string;
      commandTargetId: string;
      frameRoleId: string;
      qualifiedName: string;
      isDefault: boolean;
      mountedHere: boolean;
      mountedElsewhere?: boolean;
    }>;
  }>,
  draftCustomTcps: (componentId: string) => readonly string[] = () => [],
  showOtherRobots = false,
): AvailableMotionTcp[] {
  const rows = descriptors.flatMap((descriptor) => descriptor.tcps.filter(
    (tcp) => showOtherRobots || !tcp.mountedElsewhere,
  ));
  const rolesOf = (componentId: string, targetId: string) => boundTargets.filter((target) => (
    target.controllable_component_id === componentId && target.command_target_id === targetId
  )).map((target) => target.target_role_id);
  const applied = rows.flatMap((tcp) => rolesOf(tcp.controllableComponentId, tcp.commandTargetId)
    .map((targetRole): AvailableMotionTcp => ({
      targetRole,
      frameRole: tcp.frameRoleId,
      qualifiedName: tcp.qualifiedName,
      isDefault: tcp.isDefault,
      mountedElsewhere: tcp.mountedElsewhere,
    })));
  // A component's custom TCPs join each of its targets that already carries TCPs.
  const targets = new Map(rows.map((tcp) => [
    JSON.stringify([tcp.controllableComponentId, tcp.commandTargetId]),
    [tcp.controllableComponentId, tcp.commandTargetId] as const,
  ]));
  const pending = [...targets.values()].flatMap(([componentId, targetId]) => draftCustomTcps(componentId)
    .filter((tcpId) => !rows.some((tcp) => (
      tcp.controllableComponentId === componentId && tcp.frameRoleId === tcpId)))
    .flatMap((tcpId) => rolesOf(componentId, targetId).map((targetRole): AvailableMotionTcp => ({
      targetRole,
      frameRole: tcpId,
      qualifiedName: `${componentId}.${tcpId}`,
      isDefault: false,
      draftOnly: true,
    }))));
  return [...applied, ...pending];
}

export type TcpSelector =
  | { kind: 'current' }
  | { kind: 'default_of_role'; role: string }
  | { kind: 'entry'; target_role: string; frame_role: string };

/** Whether a visible Applied TCP owner lacks a role assigned in the Program. */
export function hasUnrepresentedTcpOwner(
  program: Pick<ProgramAuthoringDocument, 'target_roles'>,
  boundTargets: ReadonlyArray<{ target_role_id: string; controllable_component_id: string; command_target_id: string }>,
  descriptor: { tcps: ReadonlyArray<{
    controllableComponentId: string; commandTargetId: string; mountedElsewhere: boolean;
  }> } | undefined,
  showOtherRobots: boolean,
): boolean {
  return descriptor?.tcps.some((tcp) => (showOtherRobots || !tcp.mountedElsewhere)
    && !boundTargets.some((target) => target.target_role_id in program.target_roles
      && target.controllable_component_id === tcp.controllableComponentId
      && target.command_target_id === tcp.commandTargetId)) ?? false;
}

export function tcpSelectorKey(selector: TcpSelector): string {
  if (selector.kind === 'current') return JSON.stringify(['current']);
  if (selector.kind === 'default_of_role') return JSON.stringify(['default_of_role', selector.role]);
  return motionEndpointChoiceKey(selector.target_role, selector.frame_role);
}

export function programTcpSelectorChoices(
  program: Pick<ProgramAuthoringDocument, 'target_roles'>,
  boundTargets: ReadonlyArray<{ target_role_id: string; controllable_component_id: string; command_target_id: string }>,
  descriptor: { tcps: ReadonlyArray<{
    controllableComponentId: string; commandTargetId: string; frameRoleId: string;
    qualifiedName: string; isDefault: boolean; mountedHere: boolean; mountedElsewhere: boolean;
  }> } | undefined,
  showOtherRobots: boolean,
  draftCustomTcps: (componentId: string) => readonly string[] = () => [],
): Array<{ key: string; label: string; selector: TcpSelector }> {
  const available = availableMotionTcps(boundTargets, descriptor ? [descriptor] : [], draftCustomTcps, showOtherRobots);
  const entries = programMotionEndpointChoices(program, () => null, available);
  const availableKeys = new Set(available.map((tcp) => motionEndpointChoiceKey(tcp.targetRole, tcp.frameRole)));
  const visibleEntries = descriptor === undefined ? entries : entries.filter((entry) => availableKeys.has(entry.key));
  const hiddenRoles = new Set<string>();
  if (!showOtherRobots && descriptor !== undefined) {
    for (const target of boundTargets) {
      const rows = descriptor.tcps.filter((tcp) => tcp.controllableComponentId === target.controllable_component_id
        && tcp.commandTargetId === target.command_target_id);
      if (rows.length > 0 && rows.every((tcp) => tcp.mountedElsewhere)) hiddenRoles.add(target.target_role_id);
    }
  }
  return [
    { key: tcpSelectorKey({ kind: 'current' }), label: 'Current', selector: { kind: 'current' } },
    ...Object.keys(program.target_roles).filter((role) => !hiddenRoles.has(role)).sort().map((role) => ({
      key: tcpSelectorKey({ kind: 'default_of_role', role }), label: `Default TCP of ${role}`,
      selector: { kind: 'default_of_role' as const, role },
    })),
    ...visibleEntries.map((choice) => ({
      key: choice.key, label: choice.label,
      selector: { kind: 'entry' as const, target_role: choice.targetRole, frame_role: choice.frameRole },
    })),
  ];
}

/** Current selector; omitted move/planning selection means Current. */
export function motionEndpointKeyOfNode(node: ProgramTreeNode, parameterId = 'tcp_endpoint'): string | null {
  if (!Object.prototype.hasOwnProperty.call(node.parameters, parameterId)) {
    return parameterId === 'tcp_endpoint' ? tcpSelectorKey({ kind: 'current' }) : null;
  }
  const endpoint = objectValue(node.parameters[parameterId]);
  if (endpoint === null) return null;
  if (endpoint.kind === 'current' && Object.keys(endpoint).length === 1) {
    return tcpSelectorKey({ kind: 'current' });
  }
  if (endpoint.kind === 'default_of_role' && Object.keys(endpoint).length === 2 && typeof endpoint.role === 'string') {
    return tcpSelectorKey({ kind: 'default_of_role', role: endpoint.role });
  }
  if (endpoint.kind === 'entry' && Object.keys(endpoint).length === 3
    && typeof endpoint.target_role === 'string' && typeof endpoint.frame_role === 'string') {
    return tcpSelectorKey({ kind: 'entry', target_role: endpoint.target_role, frame_role: endpoint.frame_role });
  }
  return null;
}

/** Replace one authored TCP selector while preserving unrelated values. */
export function buildMotionEndpointMutation(
  node: ProgramTreeNode,
  endpoint: TcpSelector | null,
  parameterId = 'tcp_endpoint',
) {
  const parameters = { ...node.parameters };
  const valueSources = { ...(node.value_sources ?? {}) };
  delete valueSources[parameterId];
  if (endpoint === null) delete parameters[parameterId];
  else parameters[parameterId] = endpoint as unknown as JsonValue;
  return {
    mutation: 'tree_draft_update_node_params' as const,
    args: {
      instance_id: node.instance_id,
      expected_node_id: node.node_id,
      merge: false,
      parameters,
      value_sources: valueSources as unknown as JsonValue,
    },
  };
}
