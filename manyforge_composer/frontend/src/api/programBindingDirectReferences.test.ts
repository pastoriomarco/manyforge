import { describe, expect, it } from 'vitest';
import nativeFixture from '../../../../manyforge_deployment/resources/program_binding_lite6_native.fixture.json';
import { parseProgramBindingTransport } from './programBindingTransport.generated';

type Row = Record<string, unknown>;
const native = nativeFixture as unknown as Row;

// The native v3 fixture must not be converted or rehashed to stand in for v4.
describe.skipIf(native.api_version !== 'manyforge.program_binding.v4')('native v4 direct references', () => {
  it.each(['target_bindings', 'frame_bindings', 'collision_body_bindings'])(
    'rejects missing component and extra fields in %s', (collection) => {
      parseProgramBindingTransport(native, 'binding');
      for (const fault of ['missing_component', 'unknown_field']) {
        const value = structuredClone(native);
        const binding = (value[collection] as Row[])[0];
        const reference = binding[collection === 'target_bindings' ? 'target' : 'reference'] as Row;
        if (fault === 'missing_component') delete reference.component_id;
        else reference.unknown = true;
        expect(() => parseProgramBindingTransport(value, 'binding')).toThrow();
      }
    },
  );
});
