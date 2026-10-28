import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';

import type { ProgramWorkspaceController } from '../hooks/useProgramWorkspace';
import type { ScenarioSourceDiagnostic } from '../types/api';
import { programDocumentHeader } from './ProgramDocumentControls';

function notice(diagnostics: ScenarioSourceDiagnostic[]): string {
  const controller = {
    state: { workspace: null },
    scenarioSourceDiagnostics: diagnostics,
  } as unknown as ProgramWorkspaceController;
  return renderToStaticMarkup(<>{programDocumentHeader(controller).notice}</>);
}

describe('programDocumentHeader', () => {
  it('warns that Scenario files were edited outside ManyForge', () => {
    const markup = notice([{
      code: 'edited_outside_manyforge',
      sourceId: 'member:program.yaml',
      message: 'Edited outside ManyForge since the last Save',
    }]);
    expect(markup).toContain('severity-warning');
    expect(markup).toContain('Scenario files were edited outside ManyForge.');
    expect(markup).toContain('member:program.yaml: Edited outside ManyForge since the last Save');
  });

  it('keeps the referenced-source notice for source changes only', () => {
    const markup = notice([{
      code: 'scenario_source_changed',
      sourceId: 'component:arm:urdf',
      message: 'Referenced source content changed',
    }]);
    expect(markup).toContain('Referenced sources changed since the last Save.');
    expect(notice([])).toBe('');
  });
});
