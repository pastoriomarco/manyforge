import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';

import App from './App';

describe('Composer shell continuity', () => {
  it('renders the complete structural shell in dependency order within the initial budget', () => {
    const started = performance.now();
    const markup = renderToStaticMarkup(<App />);
    const elapsedMs = performance.now() - started;

    const workspace = markup.indexOf('Program workspace');
    const lowerGrid = markup.indexOf('class="lower-grid"');
    const footer = markup.indexOf('class="app-footer"');
    expect(markup).toContain('ManyForge Composer');
    expect(markup).toContain('Structural mode');
    expect(workspace).toBeGreaterThan(0);
    expect(lowerGrid).toBeGreaterThan(workspace);
    expect(footer).toBeGreaterThan(lowerGrid);
    expect(markup).toContain('Composer owns local drafts and presentation only.');
    expect(elapsedMs).toBeLessThan(2000);
  });
});
