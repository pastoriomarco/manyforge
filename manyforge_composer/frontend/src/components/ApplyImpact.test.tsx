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
        executable: false,
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
    expect(markup).toContain('The Program will not be executable after this Apply.');
    expect(markup).toContain('<li title="arm is removed"><code>arm/arm</code>: the component is not in the Deployment</li>');
    expect(markup).toContain('Affected steps: move_1, grip_1; groups: main');
  });

  it('lists the collision rule changes with their resolved members', () => {
    const markup = renderToStaticMarkup(<ApplyImpact preview={applyPreviewFixture([], {
      collision_rules: [
        {
          rule_id: 'everything', change: 'changed',
          now: { rule: { rule_id: 'everything', clearance_m: 0.002 }, a: [], b: [] },
          was: { rule: { rule_id: 'everything', clearance_m: 0.001 }, a: [], b: [] },
        },
        {
          rule_id: 'pads_part', change: 'added',
          now: {
            rule: { rule_id: 'pads_part', a: [], b: [], contact: {} },
            a: [{ kind: 'link', component_id: 'gripper', link: 'left_pad' },
              { kind: 'link', component_id: 'gripper', link: 'right_pad' }],
            b: [{ kind: 'attached_object', component_id: 'gripper' }],
          },
          was: null,
        },
        {
          rule_id: 'old', change: 'removed', now: null,
          was: { rule: { rule_id: 'old', a: [], b: [], clearance_m: 0.01 }, a: [], b: [{ kind: 'scene_objects' }] },
        },
      ],
    })} />);
    expect(markup).toContain('confirming it confirms these');
    expect(markup).toContain('Rule <code>everything</code> changes: now everything else, clearance 0.002 m; was everything else, clearance 0.001 m');
    expect(markup).toContain('Rule <code>pads_part</code> added: now gripper/left_pad, gripper/right_pad ↔ object carried by gripper, may touch');
    expect(markup).toContain('Rule <code>old</code> removed: was nothing yet ↔ Scene objects, clearance 0.01 m');
  });

  it('reports depths that select a mesh that is not closed', () => {
    const markup = renderToStaticMarkup(<ApplyImpact preview={applyPreviewFixture([], {
      mesh_depth_reports: [
        {
          code: 'mesh_not_closed', collision_rule_id: 'pads_part',
          member: { kind: 'link', component_id: 'gripper', link: 'left_pad' },
          mesh_uri: 'asset://gripper/pad.stl',
        },
        {
          code: 'mesh_not_closed', signal_id: 'tool_io/TI0',
          member: { kind: 'scene_object', scene_resource_id: 'endplate' },
          mesh_uri: 'meshes/endplate.stl',
        },
      ],
    })} />);
    expect(markup).toContain('These depths need closed meshes and cannot be measured');
    expect(markup).toContain('Rule <code>pads_part</code>: gripper/left_pad, mesh <code>asset://gripper/pad.stl</code> is not closed');
    expect(markup).toContain('Input <code>tool_io/TI0</code>: Scene object endplate, mesh <code>meshes/endplate.stl</code> is not closed');
  });

  it('shows no Program warning while the Program stays executable', () => {
    const markup = renderToStaticMarkup(<ApplyImpact preview={applyPreviewFixture([])} />);
    expect(markup).not.toContain('will not be executable');
    expect(markup).not.toContain('Attachments change');
    expect(markup).not.toContain('Collision rule changes');
    expect(markup).not.toContain('Meshes not closed');
  });
});
