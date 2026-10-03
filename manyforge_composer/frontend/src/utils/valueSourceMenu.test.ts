import { describe, expect, it, vi } from 'vitest';

import { valueSourceMenuEntries } from '../components/property/ValueSourceField';
import type {
  BlackboardKeyDeclaration,
  CatalogEntry,
  CatalogJsonSchema,
  ProgramParameterDeclaration,
} from '../types/api';
import { valueSourceMenuModel, type ValueSourceMenuInput } from './valueSourceMenu';

const NUMBER: CatalogJsonSchema = { type: 'number' };
const POSE: CatalogJsonSchema = {
  type: 'object',
  properties: {
    frame_ref: { type: 'object', properties: { kind: { type: 'string', enum: ['cell_frame'] } } },
    position_m: { type: 'array', minItems: 3, maxItems: 3, items: NUMBER },
    orientation: { type: 'object', properties: {} },
  },
};

const entry: CatalogEntry = {
  id: 'motion.move_to_pose',
  name: 'Move to pose',
  category: 'motion',
  description: '',
  stability: 'stable',
  kind: 'leaf',
  childrenMin: 0,
  childrenMax: 0,
  parameters: ['target_role', 'goal', 'target_reference', 'velocity_scale', 'target_mode'].map((id) => ({
    id,
    required: false,
    schema: {},
    schemaHash: `sha256:${id}`,
    hasDefault: false,
  })),
  requiredCapabilities: [{
    capabilityId: 'motion', scope: 'target', effectful: true, targetParameter: 'target_role',
  }],
  execution: { blocking: true, cancellable: true, ownerApi: 'motion' },
  runners: {},
  tags: [],
  minProgramProfile: null,
  blackboard: { reads: [], writes: [] },
  ownerContext: null,
  outcomes: {},
  eventFamily: null,
};

const parameters: ProgramParameterDeclaration[] = [
  { parameter_id: 'grasp_z', type: 'float64', default: 0.1 },
  { parameter_id: 'count', type: 'int64', default: 2 },
  { parameter_id: 'cycle_speed', type: 'float64', default: 0.5 },
];
const keys: BlackboardKeyDeclaration[] = [
  { key: 'place_pose', type: 'stored_pose', access: 'read_write', persistence: 'run' },
  { key: 'offset', type: 'float64', access: 'read', persistence: 'run' },
  { key: 'sink', type: 'float64', access: 'write', persistence: 'run' },
  {
    key: 'plan', type: 'pick_planning_result', access: 'read', persistence: 'run',
    fields: [{ field: 'approach_pose', type: 'stored_pose' }],
  },
];
const goal = { position_m: [0.3, 0.1, 0], orientation: { rpy_deg: [0, 0, 0] } };

function model(overrides: Partial<ValueSourceMenuInput>) {
  return valueSourceMenuModel({
    entry, path: 'goal', schema: POSE, sources: undefined, literals: { goal },
    parameters, keys, ...overrides,
  });
}

function labels(entries: ReturnType<typeof valueSourceMenuEntries>): string[] {
  return entries.map((item) => item === 'separator' ? '—' : item.label);
}

const actions = { typeValues: vi.fn(), use: vi.fn(), editFormula: vi.fn() };

describe('value-source menu', () => {
  it('offers pose keys and fields for a whole pose, and no formula', () => {
    const pose = model({})!;
    expect(pose.keyChoices.map((choice) => choice.label)).toEqual(['key.place_pose', 'key.plan.approach_pose']);
    expect(pose.parameterChoices).toEqual([]);
    expect(pose.formulaType).toBeNull();
    expect(labels(valueSourceMenuEntries(pose, actions))).toEqual(['Type values', 'Use key…', 'Use parameter…']);
  });

  it('offers readable numbers and a formula for one coordinate', () => {
    const z = model({ path: 'goal.position_m[2]', schema: NUMBER })!;
    expect(z.keyChoices.map((choice) => choice.label)).toEqual(['key.offset']);
    expect(z.parameterChoices.map((choice) => choice.label)).toEqual(['param.grasp_z', 'param.cycle_speed']);
    expect(z.formulaType).toBe('float64');
    expect(z.unavailable).toBeNull();
    expect(labels(valueSourceMenuEntries(z, actions)))
      .toEqual(['Type values', 'Use key…', 'Use parameter…', '—', 'Add formula']);
    const integer = model({ path: 'goal.position_m[2]', schema: { type: 'integer' } })!;
    expect(integer.formulaType).toBe('int64');
    expect(integer.parameterChoices.map((choice) => choice.label)).toEqual(['param.count']);
  });

  it('edits or removes a current formula and marks the current binding', () => {
    const current = model({
      path: 'goal.position_m[2]',
      schema: NUMBER,
      sources: { 'goal.position_m[2]': { kind: 'expression', expression: 'param.grasp_z' } },
    })!;
    const entries = valueSourceMenuEntries(current, actions);
    expect(labels(entries)).toEqual(['Type values', 'Use key…', 'Use parameter…', '—', 'Edit formula', 'Remove formula']);
    const bound = model({
      path: 'goal.position_m[2]',
      schema: NUMBER,
      sources: { 'goal.position_m[2]': { kind: 'program_parameter', reference: 'grasp_z' } },
    })!;
    const parameterMenu = valueSourceMenuEntries(bound, actions)[2];
    expect(parameterMenu !== 'separator' && parameterMenu.items?.[0]).toMatchObject({
      label: 'param.grasp_z', disabled: true, title: 'Current source',
    });
  });

  it('offers formulas for bool and string values, never for a choice', () => {
    expect(model({ path: 'velocity_scale', schema: { type: 'boolean' } })?.formulaType).toBe('bool');
    expect(model({ path: 'goal.position_m[0]', schema: { type: 'boolean' } })?.formulaType).toBe('bool');
    expect(model({ path: 'velocity_scale', schema: { type: 'string' } })?.formulaType).toBe('string');
    expect(model({
      path: 'target_mode',
      schema: { type: 'string', enum: ['absolute', 'relative'] },
      keys: [{ key: 'mode', type: 'string', access: 'read', persistence: 'run' }],
    })).toMatchObject({ formulaType: null, keyChoices: [{ label: 'key.mode' }] });
  });

  it('never sources effect selectors, variant selectors, frames or quaternion components', () => {
    expect(model({ path: 'target_role', schema: { type: 'string' } })).toBeNull();
    expect(model({ path: 'target_reference.type', schema: { type: 'string', enum: ['pose', 'frame_role'] } }))
      .toBeNull();
    // Nested text and identifiers (a pose's frame roles included) stay literal.
    expect(model({ path: 'goal.frame_ref.kind', schema: { type: 'string' } })).toBeNull();
    expect(model({ path: 'goal.frame_ref', schema: POSE.properties!.frame_ref! })).toBeNull();
    expect(model({ path: 'goal.orientation.xyzw[3]', schema: NUMBER })).toBeNull();
    expect(model({
      path: 'goal.frame_ref.target_role',
      schema: { type: 'string', 'x-manyforge-semantic-type': 'target_role' },
    })).toBeNull();
    // A value nothing could replace offers no menu, so the enclosing value's menu opens.
    expect(model({ path: 'goal.position_m', schema: POSE.properties!.position_m! })).toBeNull();
  });

  it('keeps sources for values that exist, and offers only the Program motion default', () => {
    const missing = model({ path: 'goal.position_m[2]', schema: NUMBER, literals: {} })!;
    expect(missing.unavailable).toMatch(/^Type a value here first/);
    const entries = valueSourceMenuEntries(missing, actions);
    expect(entries.filter((item) => item !== 'separator' && item.disabled).map((item) =>
      item !== 'separator' && item.label)).toEqual(['Type values', 'Use key…', 'Use parameter…', 'Add formula']);
    const speed = model({
      path: 'velocity_scale', schema: NUMBER, literals: {}, preferredParameter: 'cycle_speed',
    })!;
    expect(speed.unavailable).toBeNull();
    expect(speed.parameterChoices.map((choice) => choice.label)).toEqual(['param.cycle_speed']);
  });

  it('offers every key and parameter of a template variable type for a prompt-template variable', () => {
    const variable: CatalogJsonSchema = {
      anyOf: [
        { type: 'boolean' },
        { type: 'number' },
        { type: 'integer' },
        { type: 'string' },
        { type: 'array', items: { type: 'string' } },
      ],
    };
    const query: CatalogEntry = {
      ...entry,
      id: 'assistant.query_model',
      parameters: [{ ...entry.parameters[0]!, id: 'template_variables' }],
      requiredCapabilities: [],
    };
    const menu = model({
      entry: query,
      path: 'template_variables.count',
      schema: variable,
      literals: { template_variables: { count: 1 } },
    })!;
    expect(menu.keyChoices.map((choice) => choice.label)).toEqual(['key.offset']);
    expect(menu.parameterChoices.map((choice) => choice.label))
      .toEqual(['param.grasp_z', 'param.count', 'param.cycle_speed']);
    // The variable's type comes from its template, so no formula type is known here.
    expect(menu.formulaType).toBeNull();
  });
});
