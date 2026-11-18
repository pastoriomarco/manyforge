import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';

import type { ProgramRuntimeController } from '../hooks/useProgramRuntime';
import { ReloadInitialSceneOption, ResetsRunWarning } from './ScenarioActionControls';

const persistentValues = {
  read: vi.fn(() => new Promise(() => undefined)),
} as unknown as ProgramRuntimeController['persistentValues'];

function option(patch: Partial<Parameters<typeof ReloadInitialSceneOption>[0]> = {}) {
  return renderToStaticMarkup(
    <ReloadInitialSceneOption
      action="apply" resetsRun={false} checked={false} onChange={vi.fn()}
      persistentKeys={false} persistentValues={persistentValues} {...patch}
    />,
  );
}

describe('Reload initial scene in the Apply and Revert confirmation', () => {
  it('is unchecked by default and says that it performs a Reset, as the Reset dialog lists it', () => {
    const markup = option();
    expect(markup).toContain('name="reloadInitialScene"');
    expect(markup).not.toContain('checked=""');
    expect(markup).toContain('Reload initial scene');
    expect(markup).toContain('After the Apply, a Reset reloads the initial scene.');
    expect(markup).toContain('Scene objects, including carried ones, are replaced by the Program&#x27;s initial Scene.');
    expect(markup).not.toContain('Persistent keys');
    expect(markup).not.toContain('Robot positions stay as they are.');
  });

  it('names the persistent-key reset and what stays when checked', () => {
    const markup = option({ checked: true, persistentKeys: true });
    expect(markup).toContain('checked=""');
    expect(markup).toContain('Persistent keys return to their initial values.');
    expect(markup).toContain('Robot positions stay as they are.');
  });

  it('says a second Reset follows the run\'s Reset, for Apply and Revert alike', () => {
    expect(option({ resetsRun: true })).toContain('After the Apply, a second Reset reloads the initial scene.');
    expect(option({ action: 'revert' })).toContain('After the Revert, a Reset reloads the initial scene.');
  });

  it('warns that a Revert Resets the current run, as Apply does', () => {
    expect(renderToStaticMarkup(<ResetsRunWarning action="revert" />))
      .toContain('Reverting Resets the current run.');
    expect(renderToStaticMarkup(<ResetsRunWarning />)).toContain('exactly this Draft is Applied.');
  });
});
