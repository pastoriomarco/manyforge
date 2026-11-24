import type { JsonValue, ProgramTreeNode, ProgramValueSource } from '../types/api';

/** One condition the running Program waits for, and the nodes waiting on it (OD-354). */
export interface ProgramWait {
  condition: string;
  nodeInstanceIds: string[];
}

function valueText(value: JsonValue | undefined): string {
  if (value === undefined) return '?';
  return typeof value === 'string' ? value : JSON.stringify(value);
}

function sourceText(source: ProgramValueSource): string {
  switch (source.kind) {
    case 'blackboard_key':
      return `key ${source.reference}`;
    case 'program_parameter':
      return `parameter ${source.reference}`;
    case 'blackboard_field':
      return `key ${source.reference}.${source.field}`;
    case 'expression':
      return source.expression;
  }
}

/** A parameter as written: its value source when it has one, else its value. */
function parameterText(node: ProgramTreeNode, id: string): string {
  const source = node.value_sources?.[id];
  return source === undefined ? valueText(node.parameters[id]) : sourceText(source);
}

function objectMember(value: JsonValue | undefined, member: string): string | null {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) return null;
  const field = value[member];
  return typeof field === 'string' ? field : null;
}

function waitCondition(node: ProgramTreeNode): string | null {
  switch (node.node_id) {
    case 'logic.wait_for_value': {
      const operator = node.parameters.operator === undefined ? '==' : valueText(node.parameters.operator);
      const tolerance = node.parameters.tolerance === undefined && node.value_sources?.tolerance === undefined
        ? '' : ` ± ${parameterText(node, 'tolerance')}`;
      return `${parameterText(node, 'key')} ${operator} ${parameterText(node, 'value')}${tolerance}`;
    }
    case 'io.wait_for_input': {
      const signal = node.parameters.signal;
      const component = objectMember(signal, 'component_id');
      const name = objectMember(signal, 'signal_name');
      const label = component !== null && name !== null ? `${component}/${name}` : parameterText(node, 'signal');
      const edge = node.parameters.edge === undefined ? '' : ` (${valueText(node.parameters.edge)} edge)`;
      return `input ${label} = ${parameterText(node, 'expected_value')}${edge}`;
    }
    case 'gripper.wait_for_status': {
      const component = objectMember(node.parameters.target, 'component_id');
      return `gripper ${component ?? parameterText(node, 'target')} ${parameterText(node, 'expected_state')}`;
    }
    default:
      return null;
  }
}

/** The conditions of the running wait nodes, grouped by condition, in tree order. */
export function activeProgramWaits(
  tree: ProgramTreeNode,
  nodeStatuses: Readonly<Record<string, string>>,
): ProgramWait[] {
  const waits = new Map<string, string[]>();
  const visit = (node: ProgramTreeNode) => {
    if (nodeStatuses[node.instance_id] === 'running') {
      const condition = waitCondition(node);
      if (condition !== null) waits.set(condition, [...(waits.get(condition) ?? []), node.instance_id]);
    }
    node.children.forEach(visit);
  };
  visit(tree);
  return [...waits].map(([condition, nodeInstanceIds]) => ({ condition, nodeInstanceIds }));
}
