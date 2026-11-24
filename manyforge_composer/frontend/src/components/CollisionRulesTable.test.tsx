import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';

import type { JsonValue } from '../types/api';
import type { CollisionRuleChoices, CollisionRuleScope } from '../utils/collisionRules';
import { CollisionRulesTable } from './CollisionRulesTable';

const choices: CollisionRuleChoices = {
  componentIds: ['lite6_robot', 'lite_gripper'],
  bodies: [
    { component_id: 'lite_gripper', collision_body_id: 'gripper_collision' },
    { component_id: 'lite_gripper', collision_body_id: 'gripper_fingers' },
  ],
  sceneObjectIds: ['table'],
  sceneResourceKeys: ['trial.probe_object'],
};

function render(value: JsonValue | undefined, scope: CollisionRuleScope, offered = choices): string {
  return renderToStaticMarkup(
    <CollisionRulesTable value={value} scope={scope} choices={offered} disabled={false} onCommit={vi.fn()} />,
  );
}

describe('collision rules table', () => {
  it('shows Everything else first and one row per rule with its two selections and value', () => {
    const markup = render([
      { rule_id: 'everything', clearance_m: 0.002 },
      {
        rule_id: 'tip_on_part',
        a: [{ kind: 'active_tcp_contact_bodies' }],
        b: [{ kind: 'scene_object', blackboard_key: 'trial.probe_object' }],
        contact: { max_penetration_m: 0.002, end_from_fraction: 0.8 },
      },
    ], 'node');
    expect(markup).toContain('Everything else: clearance');
    expect(markup).toMatch(/aria-label="Everything else clearance"[^>]*value="0\.002"/);
    expect(markup.indexOf('Everything else: clearance')).toBeLessThan(markup.indexOf('Collision rule tip_on_part'));
    expect(markup).toContain('aria-label="tip_on_part Selection A"');
    expect(markup).toContain('aria-label="tip_on_part Selection B"');
    expect(markup).toContain('<option value="[&quot;key&quot;,&quot;trial.probe_object&quot;]" selected="">Key trial.probe_object</option>');
    expect(markup).toContain('<option value="contact" selected="">May touch</option>');
    expect(markup).toMatch(/aria-label="tip_on_part deepest penetration"[^>]*value="0\.002"/);
    expect(markup).toMatch(/aria-label="tip_on_part only at the end of the move"[^>]*checked=""/);
    expect(markup).toMatch(/aria-label="tip_on_part end from fraction"[^>]*value="0\.8"/);
    expect(markup).toContain('contact at the start or before it is refused');
    // A node has no leg; only pick and place sequences do.
    expect(markup).not.toContain('aria-label="tip_on_part leg"');
    expect(markup).toContain('Remove collision rule');
    expect(markup).toContain('Add collision rule');
  });

  it('shows whole-move contact (fraction 0) without a depth as unchecked depth and no window', () => {
    const markup = render([{
      rule_id: 'part_in_slider',
      a: [{ kind: 'attached_object', component_id: 'lite_gripper' }],
      b: [{ kind: 'scene_object', scene_resource_id: 'table' }],
      contact: { end_from_fraction: 0 },
    }], 'node');
    expect(markup).toMatch(/aria-label="part_in_slider deepest penetration"[^>]*placeholder="not checked"[^>]*value=""/);
    expect(markup).not.toContain('aria-label="part_in_slider end from fraction"');
    expect(markup).toContain('The pair may touch during the whole move, from its first sample. The depth is not checked.');
    expect(markup).toContain('<option value="[&quot;id&quot;,&quot;table&quot;]" selected="">table</option>');
  });

  it('shows a contact value without a fraction as the default end window', () => {
    const markup = render([{
      rule_id: 'tip',
      a: [{ kind: 'active_tcp_contact_bodies' }],
      b: [{ kind: 'scene_object', scene_resource_id: 'table' }],
      contact: {},
    }], 'node');
    expect(markup).toMatch(/aria-label="tip only at the end of the move"[^>]*checked=""/);
    expect(markup).toMatch(/aria-label="tip end from fraction"[^>]*value="0\.8"/);
    expect(markup).toContain('contact at the start or before it is refused');
  });

  it('shows the release fields with the evidence labels', () => {
    const release = (evidence: string) => render([{
      rule_id: 'lift', leg: 'retreat',
      a: [{ kind: 'attached_object', component_id: 'lite_gripper' }],
      b: [{ kind: 'scene_object', scene_resource_id: 'table' }],
      release: { release_evidence_mode: evidence, max_start_penetration_m: 0.03 },
    }], 'sequence');
    const signed = release('signed_penetration');
    expect(signed).toContain('<option value="signed_penetration" selected="">Precise release</option>');
    expect(signed).toContain('Lightweight release');
    expect(signed).toMatch(/aria-label="lift maximum starting penetration"[^>]*value="0\.03"/);
    expect(signed).toContain('aria-label="lift release tolerance"');
    expect(signed).toContain('aria-label="lift minimum release progress"');
    expect(signed).toContain('aria-label="lift require decreasing penetration"');
    expect(signed).toContain('<option value="retreat" selected="">Retreat</option>');
    const binary = release('binary_contact');
    expect(binary).toContain('<option value="binary_contact" selected="">Lightweight release</option>');
    expect(binary).not.toContain('aria-label="lift maximum starting penetration"');
    expect(binary).not.toContain('aria-label="lift require decreasing penetration"');
    expect(binary).toContain('aria-label="lift require separation by end"');
    expect(binary).toContain('aria-label="lift allow re-contact"');
    // A sequence has no clearance of its own: no Everything else row unless the list holds one.
    expect(binary).not.toContain('Everything else: clearance');
  });

  it('offers only cell selections on the Deployment and picks ids from the known lists', () => {
    const markup = render([
      { rule_id: 'everything', clearance_m: 0.001 },
      {
        rule_id: 'fingers_table',
        a: [{ kind: 'collision_body', component_id: 'lite_gripper', collision_body_id: 'gripper_fingers' }],
        b: [{ kind: 'scene_objects' }],
        clearance_m: 0.004,
      },
    ], 'deployment');
    expect(markup).not.toContain('Contact bodies of the move');
    expect(markup).not.toContain('<option value="scene_object"');
    expect(markup).toContain('<option value="scene_objects" selected="">All Scene objects</option>');
    expect(markup).toContain('<option value="gripper_fingers" selected="">gripper_fingers</option>');
    expect(markup).toContain('<option value="lite_gripper" selected="">lite_gripper</option>');
    expect(markup).toMatch(/aria-label="fingers_table clearance"[^>]*value="0\.004"/);
    // The Deployment's first rule is required: its clearance has no "Deployment value" placeholder.
    expect(markup).not.toContain('placeholder="Deployment value"');
  });

  it('takes typed ids while the choices are unknown and keeps a value that is not listed', () => {
    const unknown = render([{
      rule_id: 'arm_table',
      a: [{ kind: 'link', component_id: 'lite6_robot', link: 'link5' }],
      b: [{ kind: 'component', component_id: 'fixture' }],
      clearance_m: 0.01,
    }], 'node', { componentIds: null, bodies: null });
    expect(unknown).toMatch(/<input[^>]*aria-label="arm_table A1 component"[^>]*value="lite6_robot"/);
    expect(unknown).toMatch(/<input[^>]*aria-label="arm_table A1 link"[^>]*value="link5"/);
    const listed = render([{
      rule_id: 'arm_table',
      a: [{ kind: 'component', component_id: 'removed_robot' }],
      b: [{ kind: 'scene_objects' }],
      clearance_m: 0.01,
    }], 'node');
    expect(listed).toContain('<option value="removed_robot" selected="">removed_robot (not listed)</option>');
  });

  it('picks links and model groups from the cell model when it lists the component', () => {
    const offered: CollisionRuleChoices = {
      ...choices,
      links: [
        { component_id: 'lite6_robot', link: 'link5' },
        { component_id: 'lite6_robot', link: 'link6' },
      ],
      modelGroups: [{ component_id: 'lite6_robot', group: 'manipulator' }],
    };
    const markup = render([
      { rule_id: 'everything', clearance_m: 0.001 },
      {
        rule_id: 'wrist_table',
        a: [{ kind: 'link', component_id: 'lite6_robot', link: 'link6' }],
        b: [{ kind: 'model_group', component_id: 'lite6_robot', group: 'manipulator' }],
        clearance_m: 0.01,
      },
    ], 'deployment', offered);
    expect(markup).toMatch(/<select[^>]*aria-label="wrist_table A1 link"/);
    expect(markup).toContain('<option value="link6" selected="">link6</option>');
    expect(markup).toContain('<option value="link5">link5</option>');
    expect(markup).toMatch(/<select[^>]*aria-label="wrist_table B1 model group"/);
    expect(markup).toContain('<option value="manipulator" selected="">manipulator</option>');
    expect(markup).not.toMatch(/<input[^>]*aria-label="wrist_table A1 link"/);
    // A component the model does not list takes a typed name.
    const unlisted = render([{
      rule_id: 'tool_table',
      a: [{ kind: 'link', component_id: 'lite_gripper', link: 'pad' }],
      b: [{ kind: 'scene_objects' }],
      clearance_m: 0.01,
    }], 'node', offered);
    expect(unlisted).toMatch(/<input[^>]*aria-label="tool_table A1 link"[^>]*value="pad"/);
  });

  it('offers model groups on the Deployment only', () => {
    const rows: JsonValue[] = [{ rule_id: 'everything', clearance_m: 0.001 }, {
      rule_id: 'arm_table',
      a: [{ kind: 'component', component_id: 'lite6_robot' }],
      b: [{ kind: 'scene_objects' }],
      clearance_m: 0.01,
    }];
    expect(render(rows, 'deployment')).toContain('<option value="model_group">Model group (SRDF)</option>');
    expect(render(rows.slice(1), 'node')).not.toContain('value="model_group"');
  });

  it('offers the scene_resource fields of structured keys and keeps a stored one that is not listed', () => {
    const rule = (selection: JsonValue): JsonValue => [{
      rule_id: 'grasp', leg: 'descend',
      a: [{ kind: 'active_tcp_contact_bodies' }], b: [selection], contact: { end_from_fraction: 0.8 },
    }];
    const selection = { kind: 'scene_object', blackboard_key: 'planning.pick_plan', field: 'selected_object_id' };
    const offered = render(rule(selection), 'sequence', {
      ...choices, sceneResourceKeyFields: [{ key: 'planning.pick_plan', field: 'selected_object_id' }],
    });
    expect(offered).toContain('selected="">Key planning.pick_plan · selected_object_id</option>');
    expect(offered).not.toContain('(not listed)');
    expect(render(rule(selection), 'sequence'))
      .toContain('selected="">Key planning.pick_plan · selected_object_id (not listed)</option>');
  });

  it('shows a spring slider rule as the slider, side B and its end window (OD-371)', () => {
    const rows = [{
      rule_id: 'press',
      spring_slider: { component_id: 'lite_gripper', element_id: 'gripper_fingers' },
      b: [{ kind: 'scene_object', scene_resource_id: 'table' }],
      contact: { end_from_fraction: 0 },
    }];
    const markup = render(rows, 'node');
    expect(markup).toContain('aria-label="press spring slider"');
    expect(markup).toContain('<option value="gripper_fingers" selected="">gripper_fingers</option>');
    expect(markup).not.toContain('aria-label="press Selection A"');
    expect(markup).toContain('aria-label="press Selection B"');
    expect(markup).toContain('<option value="spring_slider" selected="">');
    expect(markup).not.toContain('aria-label="press deepest penetration"');
    expect(markup).toContain('may press in together up to the slider');
    expect(render([{ rule_id: 'everything', clearance_m: 0.001 }], 'deployment'))
      .not.toContain('value="spring_slider"');
  });

  it('lets a carried-object selection name its carrying body', () => {
    const markup = render([{
      rule_id: 'carried',
      a: [{ kind: 'attached_object', component_id: 'lite_gripper', collision_body_role_id: 'gripper_fingers' }],
      b: [{ kind: 'scene_objects' }],
      clearance_m: 0.001,
    }], 'node');
    expect(markup).toMatch(/aria-label="carried A1 carried by body"/);
    expect(markup).toContain('<option value="gripper_fingers" selected="">gripper_fingers</option>');
  });

  it('keeps a value that is not a list unchanged', () => {
    expect(render({ rule_id: 'everything' }, 'deployment'))
      .toContain('collision_rules is not a list; it is kept unchanged and cannot be edited here.');
  });
});
