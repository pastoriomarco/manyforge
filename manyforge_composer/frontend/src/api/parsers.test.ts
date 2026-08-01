import { describe, expect, it } from 'vitest';
import {
  StructuralContractError,
  parseCatalogResponse,
  parseSceneUrdfManifest,
} from './parsers';

function structuralManifestFixture(): Record<string, unknown> {
  return {
    robot_id: 'fixture-arm',
    model_key: 'model-key-123',
    root_link: 'base_link',
    projection: {
      kind: 'authored_neutral',
      source: 'authored_urdf',
      root_frame: 'base_link',
      position_unit: 'm',
      angle_unit: 'rad',
      quaternion_order: 'xyzw',
      joint_coordinate_policy: 'zero_independent_with_authored_mimic',
      operational_validity: 'not_asserted',
      runtime_state_used: false,
    },
    links: [
      {
        name: 'base_link',
        neutral_transform: {
          relative_to: 'base_link',
          position_m: [0, 0, 0],
          orientation_xyzw: [0, 0, 0, 1],
        },
        visuals: [],
        collisions: [],
      },
    ],
    joints: [],
  };
}

describe('parseSceneUrdfManifest', () => {
  it('accepts a complete authored-neutral projection', () => {
    const parsed = parseSceneUrdfManifest(structuralManifestFixture());

    expect(parsed.projection.runtime_state_used).toBe(false);
    expect(parsed.projection.operational_validity).toBe('not_asserted');
    expect(parsed.links[0]?.neutral_transform.relative_to).toBe('base_link');
  });

  it('rejects a projection that claims runtime state was used', () => {
    const fixture = structuralManifestFixture();
    const projection = fixture.projection as Record<string, unknown>;
    projection.runtime_state_used = true;

    expect(() => parseSceneUrdfManifest(fixture)).toThrowError(
      StructuralContractError,
    );
  });

  it('rejects links without explicit neutral transforms', () => {
    const fixture = structuralManifestFixture();
    const [link] = fixture.links as Record<string, unknown>[];
    delete link.neutral_transform;

    expect(() => parseSceneUrdfManifest(fixture)).toThrow(
      'manifest.links[0].neutral_transform must be an object',
    );
  });

  it('rejects historical runtime-authority fields', () => {
    const fixture = structuralManifestFixture();
    fixture.authority = { commandable_joint_names: ['joint1'] };

    expect(() => parseSceneUrdfManifest(fixture)).toThrow(
      'manifest must be free of runtime authority and live-state fields',
    );
  });
});

describe('parseCatalogResponse', () => {
  it('accepts the v2 compiler-product identity and entry shape', () => {
    const parsed = parseCatalogResponse({
      apiVersion: 'manyforge.node_catalog.v2',
      contentVersion: '1.0.0',
      catalogId: 'manyforge_builtin',
      catalogDigest: 'sha256:catalog',
      environmentDigest: 'sha256:environment',
      entries: [
        {
          id: 'sequence',
          name: 'Sequence',
          category: 'Control Flow',
          description: 'Runs children in order.',
          stability: 'stable',
          kind: 'composite',
          childrenMin: 1,
          childrenMax: 64,
          parameters: [],
          requiredCapabilities: [],
          execution: {
            blocking: false,
            cancellable: false,
            ownerApi: 'orchestration.control_flow',
          },
          runners: { py_trees: 'sequence_v1' },
          tags: ['control-flow'],
          minProgramProfile: null,
          blackboard: { reads: [], writes: [] },
          ownerContext: null,
          outcomes: { success: 'all children succeeded' },
          eventFamily: null,
        },
      ],
    });

    expect(parsed.apiVersion).toBe('manyforge.node_catalog.v2');
    expect(parsed.entries[0]?.execution.ownerApi).toBe(
      'orchestration.control_flow',
    );
    expect(parsed.entries[0]?.runners).toEqual({ py_trees: 'sequence_v1' });
  });

  it('does not accept a historical catalog version', () => {
    expect(() => parseCatalogResponse({ apiVersion: 'v1' })).toThrow(
      'catalog.apiVersion must be the literal "manyforge.node_catalog.v2"',
    );
  });
});
