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
  return JSON.stringify([targetRole, frameRole]);
}

/** Only the target/frame pairs the Program has explicitly declared are selectable. */
export function programMotionEndpointChoices(
  program: Pick<ProgramAuthoringDocument, 'target_roles'>,
): MotionEndpointChoice[] {
  return Object.entries(program.target_roles).flatMap(([targetRole, rawTarget]) => {
    const target = objectValue(rawTarget);
    const frames = target?.required_frame_roles;
    if (!Array.isArray(frames)) return [];
    return frames.flatMap((frameRole): MotionEndpointChoice[] => (
      typeof frameRole === 'string' && frameRole !== ''
        ? [{
          key: motionEndpointChoiceKey(targetRole, frameRole),
          targetRole,
          frameRole,
          label: `${targetRole} · ${frameRole}`,
        }]
        : []
    ));
  }).sort((left, right) => left.label.localeCompare(right.label));
}

/** Current explicitly qualified target/frame selection. */
export function motionEndpointKeyOfNode(node: ProgramTreeNode): string | null {
  const endpoint = objectValue(node.parameters.tcp_endpoint);
  if (
    endpoint !== null
    && Object.keys(endpoint).length === 2
    && typeof endpoint.target_role === 'string'
    && typeof endpoint.frame_role === 'string'
  ) {
    return motionEndpointChoiceKey(endpoint.target_role, endpoint.frame_role);
  }
  return null;
}

/** Replace the one authored TCP endpoint selector while preserving unrelated values. */
export function buildMotionEndpointMutation(
  node: ProgramTreeNode,
  endpoint: { target_role: string; frame_role: string } | null,
) {
  const parameters = { ...node.parameters };
  const valueSources = { ...(node.value_sources ?? {}) };
  delete valueSources.tcp_endpoint;
  if (endpoint === null) delete parameters.tcp_endpoint;
  else parameters.tcp_endpoint = endpoint as unknown as JsonValue;
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
