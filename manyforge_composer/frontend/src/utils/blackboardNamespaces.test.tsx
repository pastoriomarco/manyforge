import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';

import { NamespaceGroupOptions } from '../components/common/NamespaceGroupOptions';
import { ProgramDeclarationsNavigator } from '../components/ProgramDeclarationsNavigator';
import { valueSourceMenuEntries } from '../components/property/ValueSourceField';
import type {
  BlackboardKeyDeclaration,
  CatalogEntry,
  ProgramAuthoringDocument,
  ProgramTreeNode,
} from '../types/api';
import { groupByNamespace, isSystemKey, reservedNameProblem } from './blackboardNamespaces';
import { programBlackboardKeys, programSemanticChoices } from './programAuthoring';
import type { ValueSourceMenuModel } from './valueSourceMenu';

const key = (name: string, access: BlackboardKeyDeclaration['access'] = 'read_write'): BlackboardKeyDeclaration => ({
  key: name, type: 'string', access, persistence: 'run',
});

const move: ProgramTreeNode = {
  node_id: 'motion.move_to_pose', instance_id: 'approach', parameters: {}, children: [],
};
const check: ProgramTreeNode = {
  node_id: 'logic.check_value', instance_id: 'check', parameters: {}, children: [],
};

function program(keys: BlackboardKeyDeclaration[]): ProgramAuthoringDocument {
  return {
    api_version: 'manyforge.program.v2',
    program_id: 'outcomes',
    program_profile: 'composable-cell-v1',
    name: 'Outcomes',
    catalog: { api_version: 'manyforge.node_catalog.v2', content_version: '2.6.0' },
    component_roles: {},
    target_roles: {},
    blackboard: { namespaces: [{ namespace_id: 'cell', owner: 'program' }], keys },
    schema_version: 'manyforge.composer_authoring_forest.v1',
    forest: [{ node_id: 'sequence', instance_id: 'main', parameters: {}, children: [move, check] }],
    positions: {},
  };
}

const checkEntry = {
  id: 'logic.check_value',
  parameters: [{
    id: 'key', required: true, hasDefault: false, schemaHash: 'sha256:key',
    schema: { type: 'string', 'x-manyforge-semantic-type': 'blackboard_key' },
  }],
  blackboard: { reads: [{ keyParameter: 'key', valueType: 'string' }], writes: [] },
} as unknown as CatalogEntry;
const setEntry = {
  ...checkEntry,
  id: 'logic.set_value',
  blackboard: { reads: [], writes: [{ keyParameter: 'key', valueType: 'string' }] },
} as unknown as CatalogEntry;

describe('blackboard key namespaces', () => {
  it('groups by the prefix before the first dot: root first, node outcomes last', () => {
    const groups = groupByNamespace(
      ['node_outcome.a', 'zeta', 'cell.part', 'alpha', 'cell.pose.x'],
      (name) => name,
    );
    expect(groups.map((group) => [group.label, group.items])).toEqual([
      ['No namespace', ['zeta', 'alpha']],
      ['cell', ['cell.part', 'cell.pose.x']],
      ['Node outcomes', ['node_outcome.a']],
    ]);
  });

  it('adds each plain motion node outcome key as a read-only run key', () => {
    const keys = programBlackboardKeys(program([key('cell.part'), key('total')]));
    expect(keys.map((row) => row.key)).toEqual(['cell.part', 'node_outcome.approach', 'total']);
    expect(keys[1]).toMatchObject({ type: 'string', access: 'read', persistence: 'run' });
  });

  it('offers outcome keys to readers only, grouped by namespace', () => {
    const draft = program([key('cell.part'), key('total')]);
    const schema = checkEntry.parameters[0]!.schema;
    expect(programSemanticChoices(schema, check, checkEntry, 'key', draft)
      .map((choice) => [choice.group, choice.value])).toEqual([
      ['No namespace', 'total'],
      ['cell', 'cell.part'],
      ['Node outcomes', 'node_outcome.approach'],
    ]);
    expect(programSemanticChoices(schema, check, setEntry, 'key', draft)
      .map((choice) => choice.value)).toEqual(['total', 'cell.part']);
  });

  it('offers the Deployment Draft global rules as read-only interrupt_state keys (contracts 63/64)', () => {
    const draft = program([key('total')]);
    const schema = checkEntry.parameters[0]!.schema;
    expect(programSemanticChoices(schema, check, checkEntry, 'key', draft, ['light_curtain'])
      .map((choice) => [choice.group, choice.value])).toEqual([
      ['No namespace', 'total'],
      ['Interrupt states', 'interrupt_state.light_curtain'],
      ['Node outcomes', 'node_outcome.approach'],
    ]);
    // Writes stay refused: a writer is never offered the key.
    expect(programSemanticChoices(schema, check, setEntry, 'key', draft, ['light_curtain'])
      .map((choice) => choice.value)).toEqual(['total']);
  });

  it('renders select groups collapsed with a count, keeping the current value visible', () => {
    const groups = groupByNamespace(['total', 'cell.part', 'cell.pose', 'node_outcome.approach'], (name) => name);
    const render = (current: string, expanded: string[] = []) => renderToStaticMarkup(
      <select value={current} onChange={() => undefined}>
        <NamespaceGroupOptions
          groups={groups}
          expanded={new Set(expanded)}
          current={current}
          valueOf={(name) => name}
          option={(name) => <option key={name} value={name}>{name}</option>}
        />
      </select>,
    );
    const collapsed = render('');
    expect(collapsed).toContain('▸ No namespace (1)');
    expect(collapsed).toContain('▸ cell (2)');
    expect(collapsed).toContain('▸ Node outcomes (1)');
    expect(collapsed).not.toContain('value="cell.part"');
    const open = render('total', ['cell']);
    expect(open).toContain('▾ cell (2)');
    expect(open).toContain('value="cell.part"');
    expect(open).toContain('value="total"');  // The current value's group stays open.
    expect(open).not.toContain('value="node_outcome.approach"');
    // One group needs no header.
    expect(renderToStaticMarkup(
      <NamespaceGroupOptions
        groups={groupByNamespace(['a', 'b'], (name) => name)}
        expanded={new Set()}
        current=""
        valueOf={(name) => name}
        option={(name) => <option key={name} value={name}>{name}</option>}
      />,
    )).toBe('<option value="a">a</option><option value="b">b</option>');
  });

  it('nests the Use key menu by namespace, flat when there is one', () => {
    const menu = (references: string[]): ValueSourceMenuModel => ({
      path: 'value',
      current: undefined,
      keyChoices: references.map((reference) => ({
        label: `key.${reference}`, source: { kind: 'blackboard_key', reference },
      })),
      parameterChoices: [],
      formulaType: null,
      unavailable: null,
    });
    const actions = { typeValues: vi.fn(), use: vi.fn(), editFormula: vi.fn() };
    const useKey = (references: string[]) => {
      const entry = valueSourceMenuEntries(menu(references), actions)[1];
      return entry === 'separator' ? [] : entry.items ?? [];
    };
    const nested = useKey(['total', 'node_outcome.approach', 'node_outcome.retreat']);
    expect(nested.map((item) => item.label)).toEqual(['No namespace (1)', 'Node outcomes (2)']);
    expect(nested[1]!.items!.map((item) => item.label))
      .toEqual(['key.node_outcome.approach', 'key.node_outcome.retreat']);
    expect(useKey(['total', 'offset']).map((item) => item.label)).toEqual(['key.total', 'key.offset']);
  });

  it('lists outcome keys in the navigator as one collapsed, system-owned group', () => {
    const markup = renderToStaticMarkup(
      <ProgramDeclarationsNavigator
        roots={program([]).forest}
        parameters={[]}
        namespaces={[{ namespace_id: 'cell', owner: 'program' }]}
        keys={[key('cell.part'), key('total')]}
        formulaReferences={{ computedKeys: {}, nodes: {} }}
        selected={null}
        onSelect={vi.fn()}
      />,
    );
    expect(markup).toContain('<summary>Node outcomes (1)</summary>');
    expect(markup).toContain('<summary>cell (1)</summary>');
    expect(markup).not.toMatch(/<details[^>]*open/);
    expect(markup).toMatch(/<button[^>]*disabled=""[^>]*title="System-owned[^"]*"[^>]*><span><strong>node_outcome\.approach/);
  });
});

describe('reserved system names (contract 105(c))', () => {
  it('refuses the system namespaces as user key, namespace or parameter names', () => {
    for (const name of ['node_outcome', 'interrupt_state', 'node_outcome.pick', ' interrupt_state.guard']) {
      expect(reservedNameProblem(name, 'A parameter ID')).toContain('reserved for the system-owned');
    }
    expect(reservedNameProblem('cell.node_outcome', 'A blackboard key')).toBeNull();
    expect(reservedNameProblem('node_outcomes', 'A blackboard key')).toBeNull();
    expect(isSystemKey('node_outcome.pick')).toBe(true);
    expect(isSystemKey('node_outcome')).toBe(false);
  });
});
