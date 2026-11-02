import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';

import { ProgramCanvasHelp } from './ProgramCanvasHelp';

describe('ProgramCanvasHelp', () => {
  it('opens with the how-to collapsed and the legend visible', () => {
    const markup = renderToStaticMarkup(
      <ProgramCanvasHelp open onClose={() => undefined} returnFocusRef={{ current: null }} />,
    );
    expect(markup).toContain('<details class="program-canvas-help-howto">');
    expect(markup).not.toContain('<details class="program-canvas-help-howto" open');
    expect(markup).toContain('Waiting for Run');
    expect(markup).toContain('program-canvas-help-swatch status-held');
    expect(markup).toContain('bt-node-held-badge');
    expect(markup).toContain('Collapse all');
  });

  it('renders nothing while closed', () => {
    expect(renderToStaticMarkup(
      <ProgramCanvasHelp open={false} onClose={() => undefined} returnFocusRef={{ current: null }} />,
    )).toBe('');
  });
});
