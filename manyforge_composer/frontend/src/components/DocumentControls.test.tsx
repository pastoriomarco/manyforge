import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';
import { DocumentControls, scenarioName } from './DocumentControls';

describe('the header shows the open Scenario (OD-368)', () => {
  it('names the Scenario after its saved project folder', () => {
    expect(scenarioName('/projects/cell/p05_mock.scenario/program.yaml')).toBe('p05_mock');
    expect(scenarioName('/projects/cell/p05_mock.scenario/deployment.yaml')).toBe('p05_mock');
  });

  it('reads Untitled before the first Save', () => {
    expect(scenarioName(null)).toBe('Untitled');
    expect(scenarioName('/opt/manyforge/resources/Untitled.scenario/program.yaml')).toBe('Untitled');
  });

  it('shows the Scenario folder in the tooltip', () => {
    const markup = renderToStaticMarkup(
      <DocumentControls label="Deployment document controls"
        scenarioPath="/projects/cell/p05_mock.scenario/deployment.yaml" />,
    );
    expect(markup).toContain('>p05_mock</strong>');
    expect(markup).toContain('Scenario: /projects/cell/p05_mock.scenario');
    expect(markup).not.toContain('Deployment:');
  });
});
