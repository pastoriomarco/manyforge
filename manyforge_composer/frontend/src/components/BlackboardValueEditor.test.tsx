import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';

import type { JsonValue } from '../types/api';
import {
  BlackboardValueEditor,
  blackboardValueDraft,
  blackboardValueDraftIssue,
  blackboardValueEditable,
  formatBlackboardValue,
  parseBlackboardValueDraft,
} from './BlackboardValueEditor';

const RESOURCES = ['fixture', 'tray'];

function render(type: string, draft: string, sceneResourceIds = RESOURCES) {
  return renderToStaticMarkup(
    <BlackboardValueEditor
      id="value"
      label={`Value (${type})`}
      type={type}
      draft={draft}
      sceneResourceIds={sceneResourceIds}
      disabled={false}
      onDraftChange={vi.fn()}
    />,
  );
}

describe('typed blackboard values', () => {
  it('round-trips each editable type through its draft', () => {
    const pose = {
      frame_ref: { kind: 'scene_resource', scene_resource_id: 'tray' },
      position_m: [0.1, 0.2, 0.3],
      orientation: { rpy_deg: [0, 0, 90] },
    };
    for (const [type, value] of [
      ['bool', true],
      ['int64', -3],
      ['float64', 0.25],
      ['string', 'layer 2'],
      ['string_list', ['a', 'b']],
      ['scene_resource', 'fixture'],
      ['stored_pose', pose],
    ] as [string, JsonValue][]) {
      expect(blackboardValueEditable(type)).toBe(true);
      const draft = blackboardValueDraft(type, value, RESOURCES);
      expect(parseBlackboardValueDraft(type, draft, RESOURCES)).toEqual(value);
    }
    // A key without a value starts from a neutral, complete draft.
    expect(parseBlackboardValueDraft('stored_pose', blackboardValueDraft('stored_pose', undefined)))
      .toEqual({
        frame_ref: { kind: 'cell_frame' },
        position_m: [0, 0, 0],
        orientation: { rpy_deg: [0, 0, 0] },
      });
    expect(blackboardValueDraft('scene_resource', undefined, RESOURCES)).toBe('fixture');
    expect(parseBlackboardValueDraft('string_list', ' a \n\n b ')).toEqual(['a', 'b']);
    expect(blackboardValueEditable('model_response')).toBe(false);
  });

  it('names what an unfinished draft is missing instead of coercing it', () => {
    expect(blackboardValueDraftIssue('int64', '1.5')).toMatch(/whole number/);
    expect(blackboardValueDraftIssue('int64', '9007199254740993')).toMatch(/whole number/);
    expect(blackboardValueDraftIssue('float64', '')).toBe('Enter a finite number.');
    expect(blackboardValueDraftIssue('string', '')).toBe('Enter some text.');
    expect(blackboardValueDraftIssue('scene_resource', 'unknown', RESOURCES))
      .toMatch(/declared by the Program/);
    expect(blackboardValueDraftIssue('stored_pose', JSON.stringify({
      frame_ref: { kind: 'scene_resource', scene_resource_id: 'missing' },
      position_m: [0, 0, 0],
      orientation: { rpy_deg: [0, 0, 0] },
    }), RESOURCES)).toMatch(/Frame reference/);
    expect(blackboardValueDraftIssue('stored_pose', JSON.stringify({
      frame_ref: { kind: 'cell_frame' },
      position_m: [0, 0],
      orientation: { rpy_deg: [0, 0, 0] },
    }))).toMatch(/three finite numbers/);
    expect(blackboardValueDraftIssue('model_response', '{}'))
      .toBe('model_response values are written only by Program nodes.');
    expect(blackboardValueDraftIssue('bool', 'true')).toBeNull();
  });

  it('renders one control per type, with declared scene resources to choose from', () => {
    expect(render('bool', 'true')).toMatch(/<select id="value">.*<option value="true" selected="">True<\/option>/);
    expect(render('int64', '4')).toContain('type="number" step="1" value="4"');
    expect(render('float64', '0.5')).toContain('step="any"');
    expect(render('string', 'x')).toContain('type="text" value="x"');
    expect(render('string_list', 'a\nb')).toContain('placeholder="One item per line"');
    const resources = render('scene_resource', 'tray');
    expect(resources).toContain('<option value="fixture">fixture</option>');
    expect(resources).toContain('<option value="tray" selected="">tray</option>');
    expect(render('scene_resource', '', [])).toContain('The Program declares no scene resources');
    // Poses reuse the Program pose editor: frame reference, position, orientation.
    const pose = render('stored_pose', blackboardValueDraft('stored_pose', undefined));
    expect(pose).toContain('<legend>Value (stored_pose)</legend>');
    expect(pose).toContain('Frame reference');
    expect(pose).toContain('Position');
    expect(pose).toContain('Orientation representation');
    expect(render('model_response', '')).toContain('written only by Program nodes');
  });

  it('formats stored values compactly', () => {
    expect(formatBlackboardValue('int64', 3)).toBe('3');
    expect(formatBlackboardValue('string_list', [])).toBe('no items');
    expect(formatBlackboardValue('stored_pose', {
      frame_ref: { kind: 'target_frame', target_role: 'arm', frame_role: 'tcp' },
      position_m: [1, 2, 3],
      orientation: { xyzw: [0, 0, 0, 1] },
    })).toBe('arm/tcp · [1, 2, 3] m');
    expect(formatBlackboardValue('bool', undefined)).toBe('empty');
  });
});
