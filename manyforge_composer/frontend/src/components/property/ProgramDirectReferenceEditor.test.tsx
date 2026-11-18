import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';

import type { CatalogEntry, CatalogJsonSchema, ProgramAuthoringDocument, ProgramTreeNode } from '../../types/api';
import { ProgramSchemaValueEditor } from './ProgramSchemaValueEditor';

describe('direct reference picker', () => {
  it('keeps incompatible targets visible with a reason and preserves the current selection', () => {
    const markup = renderToStaticMarkup(<ProgramSchemaValueEditor
      schema={{ type: 'object', 'x-manyforge-semantic-type': 'target_reference' }}
      value={{ component_id: 'arm', command_target_id: 'motion' }}
      disabled={false}
      context={{
        entry: {} as CatalogEntry,
        node: { parameters: {} } as unknown as ProgramTreeNode,
        parameterId: 'target',
        program: { scene: [], blackboard: { keys: [] } } as unknown as ProgramAuthoringDocument,
        directReferenceChoices: { target_reference: [
          { value: { component_id: 'arm', command_target_id: 'motion' }, label: 'arm · motion' },
          { value: { component_id: 'io', command_target_id: 'port' }, label: 'io · port', reason: 'Missing motion capability' },
        ] },
      }}
      onCommit={vi.fn()}
    />);
    expect(markup).toContain('arm · motion');
    expect(markup).toContain('io · port · Missing motion capability');
    expect(markup).toContain('disabled=""');
    expect(markup).toContain('selected=""');
  });

  it('shows a reference as not checked, not invalid, until the Draft is validated', () => {
    const render = (choices?: Record<string, []>) => renderToStaticMarkup(<ProgramSchemaValueEditor
      schema={{ type: 'object', 'x-manyforge-semantic-type': 'target_reference' }}
      value={{ component_id: 'arm', command_target_id: 'motion' }}
      disabled={false}
      context={{
        entry: {} as CatalogEntry,
        node: { parameters: {} } as unknown as ProgramTreeNode,
        parameterId: 'target',
        program: { scene: [], blackboard: { keys: [] } } as unknown as ProgramAuthoringDocument,
        ...(choices === undefined ? {} : { directReferenceChoices: choices }),
      }}
      onCommit={vi.fn()}
    />);
    const unchecked = render();
    expect(unchecked).toContain('not checked yet');
    expect(unchecked).not.toContain('aria-invalid');
    const missing = render({ target_reference: [] });
    expect(missing).toContain('unavailable in the current Draft candidate');
    expect(missing).toContain('aria-invalid="true"');
  });

  it('shows an omitted motion reference as what it means, not as an unfinished frame', () => {
    const schema = { type: 'object', required: ['type'], properties: {
      type: { type: 'string', enum: ['component_frame', 'component_body', 'scene_resource', 'runtime_object', 'pose', 'cell_frame'] },
      scene_resource_id: { type: 'string' },
    } } as unknown as CatalogJsonSchema;
    const render = (path: string) => renderToStaticMarkup(<ProgramSchemaValueEditor
      schema={schema}
      value={undefined}
      disabled={false}
      path={path}
      context={{
        entry: { parameters: [] } as unknown as CatalogEntry,
        node: { parameters: { target_mode: 'relative' } } as unknown as ProgramTreeNode,
        parameterId: path,
        program: { scene: [], blackboard: { keys: [] } } as unknown as ProgramAuthoringDocument,
      }}
      onCommit={vi.fn()}
    />);
    const offset = render('offset_reference');
    expect(offset).toMatch(/<option value="" disabled="" selected="">Same as target_reference<\/option>/);
    expect(offset).not.toContain('Select a frame');
    expect(render('target_reference')).toContain('>Not set</option>');
  });
});
