import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';

import { applyPreviewFixture } from '../test/applyPreview';
import { ApplyImpact } from './ApplyImpact';

describe('ApplyImpact', () => {
  it('lists attachment changes, detached components and a Program that will not be executable', () => {
    const markup = renderToStaticMarkup(<ApplyImpact preview={applyPreviewFixture([
      { component_id: 'arm', disposition: 'remove', reasons: [], restarts: false },
      { component_id: 'camera', disposition: 'retain', reasons: [], restarts: false },
    ], {
      attachment_changed_components: ['gripper', 'camera'],
      detached_components: ['camera'],
      program: {
        executable: false, program_id: 'pick',
        unresolved_references: [{
          target: { component_id: 'arm', command_target_id: 'arm' },
          reason: 'component_missing', detail: 'arm is removed',
        }],
        affected_nodes: ['move_1', 'grip_1'], affected_execution_groups: ['main'],
      },
    })} />);
    expect(markup).toContain('Removes arm; the others keep running.');
    expect(markup).toContain('Attachments change for gripper and camera');
    expect(markup).toContain('<strong>camera</strong> stays in the cell, detached at its authored position');
    expect(markup).toContain('Program <code>pick</code> will not be executable after this Apply.');
    expect(markup).toContain('<li title="arm is removed"><code>arm/arm</code>: the component is not in the Deployment</li>');
    expect(markup).toContain('Affected steps: move_1, grip_1; groups: main');
  });

  it('shows no Program warning while the Program stays executable', () => {
    const markup = renderToStaticMarkup(<ApplyImpact preview={applyPreviewFixture([])} />);
    expect(markup).not.toContain('will not be executable');
    expect(markup).not.toContain('Attachments change');
  });
});
