import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';

import {
  buildBlackboardKeyDeclaration,
  ProgramDeclarationsEditor,
  type BlackboardKeyForm,
} from './ProgramDeclarationsEditor';

const form: BlackboardKeyForm = {
  key: ' cell.count ',
  type: 'int64',
  access: 'read_write',
  persistence: 'persistent',
  fields: [],
  description: '',
  initialText: null,
  expression: null,
};

describe('Program declarations editor', () => {
  it('refuses a key in a system namespace', () => {
    for (const key of ['node_outcome.pick', 'interrupt_state', 'interrupt_state.guard']) {
      expect(() => buildBlackboardKeyDeclaration({ ...form, key })).toThrow('reserved for the system-owned');
    }
  });

  it('saves a key without an initial value as an empty-until-written row', () => {
    expect(buildBlackboardKeyDeclaration(form)).toEqual({
      key: 'cell.count',
      type: 'int64',
      access: 'read_write',
      persistence: 'persistent',
    });
  });

  it('saves a key with an initial value validated like a parameter default', () => {
    expect(buildBlackboardKeyDeclaration({ ...form, initialText: '3' })).toEqual({
      key: 'cell.count',
      type: 'int64',
      access: 'read_write',
      persistence: 'persistent',
      initial: 3,
    });
    expect(buildBlackboardKeyDeclaration({
      ...form, type: 'string_list', persistence: 'run', initialText: '["a"]',
    })).toMatchObject({ persistence: 'run', initial: ['a'] });
    expect(() => buildBlackboardKeyDeclaration({ ...form, initialText: '1.5' }))
      .toThrow('Initial value must match int64.');
    expect(() => buildBlackboardKeyDeclaration({ ...form, type: 'stored_pose', initialText: 'null' }))
      .toThrow('Initial value must match stored_pose.');
    expect(() => buildBlackboardKeyDeclaration({ ...form, initialText: '{' }))
      .toThrow('Initial value:');
  });

  it('never saves an initial value for a type without an authored value', () => {
    expect(buildBlackboardKeyDeclaration({
      ...form, type: 'model_response', initialText: '{}',
    })).not.toHaveProperty('initial');
  });

  it('offers only the supported lifetimes and an explicit empty initial state', () => {
    const markup = renderToStaticMarkup(
      <ProgramDeclarationsEditor
        roots={[]}
        parameters={[]}
        namespaces={[]}
        keys={[]}
        formulaReferences={{ computedKeys: {}, nodes: {} }}
        busy={false}
        onMutate={vi.fn()}
      />,
    );

    expect(markup).toContain('<option value="none" selected="">empty until written</option>');
    expect(markup).toContain('<option value="computed">computed by a formula</option>');
    expect(markup).toContain('<option value="run" selected="">run</option>');
    expect(markup).toContain('starts from the initial value at each Run');
    expect(markup).not.toContain('value="node"');
    expect(markup).not.toContain('between_operations');
  });

  it('saves a computed key with its formula, never with an initial value or as persistent', () => {
    const computed = { ...form, type: 'float64', persistence: 'run' as const, expression: 'param.a / 2' };
    expect(buildBlackboardKeyDeclaration(computed)).toEqual({
      key: 'cell.count',
      type: 'float64',
      access: 'read_write',
      persistence: 'run',
      expression: 'param.a / 2',
    });
    expect(() => buildBlackboardKeyDeclaration({ ...computed, initialText: '1' }))
      .toThrow('Formula: a computed key has no initial value.');
    expect(() => buildBlackboardKeyDeclaration({ ...computed, persistence: 'persistent' }))
      .toThrow('Formula: a persistent key cannot be computed.');
    expect(() => buildBlackboardKeyDeclaration({ ...computed, type: 'stored_pose' }))
      .toThrow('Formula: stored_pose keys cannot be computed.');
  });

  it('marks a computed key in the key list', () => {
    const markup = renderToStaticMarkup(
      <ProgramDeclarationsEditor
        roots={[]}
        parameters={[]}
        namespaces={[]}
        keys={[{
          key: 'grasp_z',
          type: 'float64',
          access: 'read',
          persistence: 'run',
          expression: '-param.tube_length / 2',
        }]}
        formulaReferences={{ computedKeys: {}, nodes: {} }}
        busy={false}
        onMutate={vi.fn()}
      />,
    );
    expect(markup).toContain('<small>float64 · read · computed</small>');
  });

  it('counts declarations read only by formulas as in use', () => {
    const props = {
      roots: [{
        node_id: 'mover', instance_id: 'move', children: [], parameters: {},
        value_sources: { 'goal.position_m[2]': { kind: 'expression' as const, expression: 'key.count + 1' } },
      }],
      parameters: [{
        parameter_id: 'tube_length', type: 'float64', default: 0.1,
      }],
      namespaces: [],
      keys: [
        { key: 'count', type: 'int64', access: 'read' as const, persistence: 'run' as const, initial: 1 },
        {
          key: 'grasp_z', type: 'float64', access: 'read' as const, persistence: 'run' as const,
          expression: '-param.tube_length / 2',
        },
      ],
      formulaReferences: {
        computedKeys: { grasp_z: ['param.tube_length'] },
        nodes: { move: { 'goal.position_m[2]': ['key.count'] } },
      },
      busy: false,
      onMutate: vi.fn(),
    };
    const list = renderToStaticMarkup(<ProgramDeclarationsEditor {...props} />);
    expect(list).toContain('0 bindings · 1 formula');
    expect(list).toContain('disabled="" title="formula of key grasp_z"');
    expect(list).toContain('disabled="" title="formula in move at goal.position_m[2]"');
    // The computed key itself is read by nothing, so it stays removable.
    expect(list).toContain('title="Remove declaration"');

    const parameter = renderToStaticMarkup(
      <ProgramDeclarationsEditor
        {...props}
        showNavigation={false}
        selection={{ kind: 'parameter', id: 'tube_length' }}
      />,
    ).replaceAll('<wbr/>', '');
    expect(parameter).toMatch(/class="danger-button" disabled="">Remove parameter/);
    expect(parameter).toContain('formula of key grasp_z');
    expect(parameter).not.toContain('This parameter is not used by a tree node.');

    const key = renderToStaticMarkup(
      <ProgramDeclarationsEditor
        {...props}
        showNavigation={false}
        selection={{ kind: 'key', id: 'count' }}
      />,
    ).replaceAll('<wbr/>', '');
    expect(key).toMatch(/class="danger-button" disabled="">Remove key/);
    expect(key).toContain('formula in move at goal.position_m[2]');
  });
});
