import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';

import type {
  CatalogEntry,
  NodeEnvironmentResponse,
  ProgramParameterDeclaration,
  ProgramTreeNode,
} from '../types/api';
import { ProgramDeclarationsEditor } from './ProgramDeclarationsEditor';
import { ProgramNodePalette } from './ProgramNodePalette';
import { ProgramPropertyEditor } from './ProgramPropertyEditor';
import { ProgramTreeCanvas } from './ProgramTreeCanvas';

const sequence: CatalogEntry = {
  id: 'sequence',
  name: 'Sequence',
  category: 'control_flow',
  description: 'Run children in order.',
  stability: 'stable',
  kind: 'composite',
  childrenMin: 1,
  childrenMax: -1,
  parameters: [],
  requiredCapabilities: [],
  execution: { blocking: true, cancellable: true, ownerApi: 'program' },
  runners: { py_trees: 'sequence_v1' },
  tags: ['flow'],
  minProgramProfile: 'composable-cell-v1',
  blackboard: { reads: [], writes: [] },
  ownerContext: null,
  outcomes: {},
  eventFamily: null,
};

const timer: CatalogEntry = {
  ...sequence,
  id: 'timer',
  name: 'Timer',
  category: 'logic',
  description: 'Wait for a duration.',
  kind: 'leaf',
  childrenMin: 0,
  childrenMax: 0,
  parameters: [{
    id: 'duration_s',
    required: true,
    schema: { type: 'number', minimum: 0 },
    schemaHash: 'sha256:duration',
    hasDefault: false,
  }],
  tags: ['time'],
};

const parameter: ProgramParameterDeclaration = {
  parameter_id: 'wait_time',
  type: 'float64',
  default: 0.5,
  runtime_mutability: 'before_run',
};

const tree: ProgramTreeNode = {
  node_id: 'sequence',
  instance_id: 'root_sequence',
  parameters: {},
  children: [{
    node_id: 'timer',
    instance_id: 'settle',
    parameters: {},
    value_sources: {
      duration_s: { kind: 'program_parameter', reference: 'wait_time' },
    },
    children: [],
  }],
};

const environment: NodeEnvironmentResponse = {
  environmentDigest: 'sha256:environment',
  implementations: [sequence, timer].map((entry) => ({
    nodeId: entry.id,
    engineId: 'py_trees',
    implementationId: `${entry.id}_v1`,
    executablePackageId: 'manyforge_behavior',
    authoringAvailable: true,
    executionAvailable: false,
    reasonCodes: ['native_program_product_unavailable'],
    restorationGates: ['N7'],
  })),
  portableAuthoringAvailable: true,
  executionAvailable: false,
  reasonCodes: ['native_program_product_unavailable'],
  restorationGates: ['N7'],
  active: null,
};

describe('Program authoring panels', () => {
  it('keeps catalog authoring available while execution is separately gated', () => {
    const markup = renderToStaticMarkup(
      <ProgramNodePalette
        root={tree}
        catalogEntries={[sequence, timer]}
        environment={environment}
        selectedId="root_sequence"
        busy={false}
        onMutate={vi.fn()}
      />,
    );

    expect(markup).toContain('Authoring only · execution gate N7');
    expect(markup).toContain('draggable="false"');
    expect(markup).not.toContain('Authoring is currently unavailable');
  });

  it('renders one deterministic selected tree and exact v1 value source', () => {
    const markup = renderToStaticMarkup(
      <div>
        <ProgramTreeCanvas
          root={tree}
          catalogEntries={[sequence, timer]}
          environment={environment}
          selectedId="settle"
          busy={false}
          onSelect={vi.fn()}
          onMutate={vi.fn()}
        />
        <ProgramPropertyEditor
          root={tree}
          selectedId="settle"
          catalogEntries={[sequence, timer]}
          programParameters={[parameter]}
          blackboardKeys={[]}
          busy={false}
          onMutate={vi.fn()}
        />
      </div>,
    );

    expect(markup).toContain('aria-pressed="true"');
    expect(markup).toContain('program_parameter:wait_time');
    expect(markup).toContain('The literal is absent while this exact source is bound.');
  });

  it('reports declaration usage and prevents deleting a referenced parameter', () => {
    const markup = renderToStaticMarkup(
      <ProgramDeclarationsEditor
        root={tree}
        parameters={[parameter]}
        namespaces={[]}
        keys={[]}
        busy={false}
        onMutate={vi.fn()}
      />,
    );

    expect(markup).toContain('1 binding');
    expect(markup).toContain('title="settle.duration_s"');
    expect(markup).toContain('disabled=""');
  });

  it('lays out a broad supported tree within a practical authoring budget', () => {
    const broadTree: ProgramTreeNode = {
      ...tree,
      children: Array.from({ length: 512 }, (_, index) => ({
        node_id: 'timer',
        instance_id: `timer_${index}`,
        parameters: { duration_s: 0.1 },
        children: [],
      })),
    };
    const started = performance.now();
    const markup = renderToStaticMarkup(
      <ProgramTreeCanvas
        root={broadTree}
        catalogEntries={[sequence, timer]}
        environment={environment}
        selectedId="timer_511"
        busy={false}
        onSelect={vi.fn()}
        onMutate={vi.fn()}
      />,
    );
    const elapsedMs = performance.now() - started;

    expect(markup.match(/data-node-id=/g)).toHaveLength(513);
    expect(elapsedMs).toBeLessThan(2_000);
  });
});
