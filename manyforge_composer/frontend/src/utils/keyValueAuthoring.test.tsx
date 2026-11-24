import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';

import { ProgramSchemaValueEditor } from '../components/property/ProgramSchemaValueEditor';
import type {
  BlackboardKeyDeclaration,
  CatalogEntry,
  CatalogJsonSchema,
  ProgramAuthoringDocument,
  ProgramTreeNode,
} from '../types/api';
import { keyValueOperators, keyValueParameterRule, keyValueTyping } from './keyValueAuthoring';

// The catalog projection of `type: key_value, value_type_from_key: key`.
const VALUE_SCHEMA: CatalogJsonSchema = {
  anyOf: [
    { type: 'boolean' },
    { type: 'number' },
    { type: 'integer' },
    { type: 'string' },
  ],
  'x-manyforge-value-type-from-key': 'key',
};

const keys: BlackboardKeyDeclaration[] = [
  { key: 'index', type: 'int64', access: 'read_write', persistence: 'run' },
  { key: 'force', type: 'float64', access: 'read_write', persistence: 'run' },
  { key: 'ready', type: 'bool', access: 'read_write', persistence: 'run' },
  { key: 'label', type: 'string', access: 'read_write', persistence: 'run' },
  { key: 'pose', type: 'stored_pose', access: 'read_write', persistence: 'run' },
];

const checkValue: CatalogEntry = {
  id: 'logic.check_value',
  name: 'Check value',
  category: 'logic',
  description: '',
  stability: 'stable',
  kind: 'leaf',
  childrenMin: 0,
  childrenMax: 0,
  parameters: [
    { id: 'key', required: true, schema: { type: 'string', 'x-manyforge-semantic-type': 'blackboard_key' }, schemaHash: 'sha256:k', hasDefault: false },
    { id: 'operator', required: false, schema: { type: 'string', enum: ['==', '!=', '<', '<=', '>', '>='] }, schemaHash: 'sha256:o', hasDefault: true, default: '==' },
    { id: 'value', required: true, schema: VALUE_SCHEMA, schemaHash: 'sha256:v', hasDefault: false },
    { id: 'tolerance', required: false, schema: { type: 'number', minimum: 0 }, schemaHash: 'sha256:t', hasDefault: false },
  ],
  requiredCapabilities: [],
  execution: { blocking: false, cancellable: false, ownerApi: 'orchestration.blackboard' },
  runners: {},
  tags: [],
  minProgramProfile: null,
  blackboard: { reads: [], writes: [] },
  ownerContext: null,
  outcomes: {},
  eventFamily: null,
};

function node(parameters: ProgramTreeNode['parameters']): ProgramTreeNode {
  return { node_id: 'logic.check_value', instance_id: 'check', parameters, children: [] };
}

describe('values typed by their key', () => {
  it('takes the declared type of the chosen key', () => {
    expect(keyValueTyping({ type: 'number' }, node({}), keys)).toBeNull();
    expect(keyValueTyping(VALUE_SCHEMA, node({}), keys)?.issue).toBe('Choose the key first; the value takes its type.');
    expect(keyValueTyping(VALUE_SCHEMA, node({ key: 'gone' }), keys)?.issue).toBe('Key gone is not declared.');
    expect(keyValueTyping(VALUE_SCHEMA, node({ key: 'index' }), keys)?.schema).toEqual({ type: 'integer' });
    expect(keyValueTyping(VALUE_SCHEMA, node({ key: 'force' }), keys)?.schema).toEqual({ type: 'number' });
    // Check value accepts no pose: the catalog lists no pose branch.
    expect(keyValueTyping(VALUE_SCHEMA, node({ key: 'pose' }), keys)?.issue)
      .toBe('stored_pose keys cannot be used by this node.');
  });

  it('limits operators and the tolerance to the key type', () => {
    expect(keyValueOperators('float64')).toEqual(['==', '!=', '<', '<=', '>', '>=']);
    expect(keyValueOperators('int64')).toHaveLength(6);
    expect(keyValueOperators('bool')).toEqual(['==', '!=']);
    expect(keyValueOperators('string')).toEqual(['==', '!=']);
    const rule = (parameters: ProgramTreeNode['parameters'], id: string) =>
      keyValueParameterRule(checkValue, node(parameters), keys, id);
    expect(rule({ key: 'ready' }, 'operator')).toEqual({ choices: ['==', '!='] });
    expect(rule({ key: 'force' }, 'tolerance')).toEqual({});
    expect(rule({ key: 'force', operator: '!=' }, 'tolerance')).toEqual({});
    expect(rule({ key: 'force', operator: '<' }, 'tolerance').inapplicable)
      .toBe('A tolerance applies only to == and != comparisons.');
    expect(rule({ key: 'label' }, 'tolerance').inapplicable).toMatch(/only to number keys/);
    expect(rule({ key: 'force' }, 'value')).toEqual({});
  });

  it('offers how a wait counts held time only with a timeout', () => {
    const rule = (parameters: ProgramTreeNode['parameters'], sourced = false) =>
      keyValueParameterRule(checkValue, {
        ...node(parameters),
        ...(sourced ? { value_sources: { timeout_s: { kind: 'program_parameter', reference: 'limit' } } } : {}),
      }, keys, 'timeout_while_held');
    expect(rule({ key: 'force' }).inapplicable).toMatch(/only with a timeout_s/);
    expect(rule({ key: 'force', timeout_s: 2 })).toEqual({});
    expect(rule({ key: 'force' }, true)).toEqual({});
  });

  it('edits the value with the typed blackboard editor of the key type', () => {
    const program = {
      scene: [],
      blackboard: { namespaces: [], keys },
      parameters: [],
    } as unknown as ProgramAuthoringDocument;
    const render = (parameters: ProgramTreeNode['parameters']) => renderToStaticMarkup(
      <ProgramSchemaValueEditor
        schema={VALUE_SCHEMA}
        value={parameters.value}
        disabled={false}
        context={{ entry: checkValue, node: node(parameters), parameterId: 'value', program }}
        path="value"
        onCommit={vi.fn()}
      />,
    );
    const integer = render({ key: 'index', value: 3 });
    expect(integer).toContain('value (index: int64)');
    expect(integer).toMatch(/<input id="property-check-value-value" type="number" step="1" value="3"\/>/);
    expect(render({ key: 'ready', value: true })).toContain('<option value="true" selected="">True</option>');
    expect(render({ key: 'label', value: 'tray' })).toMatch(/type="text" value="tray"/);
    expect(render({})).toContain('Choose the key first');
  });
});

// OD-341: a pose move's motion_type decides which settings apply.
const moveToPose: CatalogEntry = {
  ...checkValue,
  id: 'motion.move_to_pose',
  name: 'Move to pose',
  category: 'motion',
  parameters: [
    { id: 'motion_type', required: false, schema: { type: 'string', enum: ['joint', 'linear'] }, schemaHash: 'sha256:m', hasDefault: true, default: 'joint' },
    { id: 'planner', required: false, schema: { type: 'object' }, schemaHash: 'sha256:p', hasDefault: false },
    { id: 'max_translation_step_m', required: false, schema: { type: 'number' }, schemaHash: 'sha256:s', hasDefault: true, default: 0.005 },
    { id: 'cartesian_timing', required: false, schema: { type: 'string', enum: ['constant_speed', 'time_optimal'] }, schemaHash: 'sha256:c', hasDefault: false },
    { id: 'translation_tol_m', required: false, schema: { type: 'number', minimum: 0 }, schemaHash: 'sha256:t', hasDefault: true, default: 0.005 },
    { id: 'rotation_tol_rad', required: false, schema: { type: 'number', minimum: 0 }, schemaHash: 'sha256:r', hasDefault: true, default: 0.05 },
    { id: 'velocity_scale', required: false, schema: { type: 'number' }, schemaHash: 'sha256:v', hasDefault: true, default: 1 },
  ],
};

describe('pose move settings follow the motion type', () => {
  const rule = (parameters: ProgramTreeNode['parameters'], id: string) =>
    keyValueParameterRule(moveToPose, {
      node_id: 'motion.move_to_pose', instance_id: 'move', parameters, children: [],
    }, keys, id);

  it('hides the linear settings on a joint move, also by default', () => {
    for (const parameters of [{}, { motion_type: 'joint' }] as ProgramTreeNode['parameters'][]) {
      expect(rule(parameters, 'max_translation_step_m').inapplicable).toMatch(/only to linear moves/);
      expect(rule(parameters, 'cartesian_timing').inapplicable).toMatch(/only to linear moves/);
      expect(rule(parameters, 'planner')).toEqual({});
      expect(rule(parameters, 'translation_tol_m')).toEqual({});
    }
  });

  it('hides the planner on a linear move and requires both tolerances', () => {
    const linear = { motion_type: 'linear' };
    expect(rule(linear, 'planner').inapplicable).toMatch(/only to joint moves/);
    expect(rule(linear, 'max_translation_step_m')).toEqual({});
    expect(rule(linear, 'cartesian_timing')).toEqual({});
    expect(rule(linear, 'translation_tol_m')).toEqual({ required: true });
    expect(rule(linear, 'rotation_tol_rad')).toEqual({ required: true });
    expect(rule(linear, 'velocity_scale')).toEqual({});
  });

  it('leaves nodes without a motion type alone', () => {
    expect(keyValueParameterRule(checkValue, node({ key: 'force' }), keys, 'planner')).toEqual({});
  });
});
