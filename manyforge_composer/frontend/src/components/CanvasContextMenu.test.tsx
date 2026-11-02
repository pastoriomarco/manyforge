import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';

import { CanvasContextMenu } from './CanvasContextMenu';

describe('canvas context menu', () => {
  it('renders a labelled menu with shortcuts, separators, submenus and disabled items', () => {
    const markup = renderToStaticMarkup(
      <CanvasContextMenu
        x={10}
        y={20}
        heading="retry_pick"
        label="Actions for retry_pick"
        onClose={vi.fn()}
        entries={[
          { label: 'Duplicate', shortcut: 'Ctrl+D', onSelect: vi.fn() },
          { label: 'Paste after', disabled: true, onSelect: vi.fn() },
          'separator',
          { label: 'Wrap in', items: [{ label: 'Inverter', onSelect: vi.fn() }] },
          { label: 'Change type', items: [] },
          { label: 'Delete', danger: true, onSelect: vi.fn() },
        ]}
      />,
    );

    expect(markup).toContain('<strong class="context-menu-heading">retry_pick</strong>');
    expect(markup).toContain('role="menu" aria-label="Actions for retry_pick"');
    expect(markup).toMatch(/<span>Duplicate<\/span><kbd>Ctrl\+D<\/kbd>/);
    expect(markup).toMatch(/disabled=""[^>]*>[^<]*<span>Paste after<\/span>/);
    expect(markup).toContain('role="separator"');
    expect(markup).toMatch(/aria-haspopup="menu" aria-expanded="false"[^>]*><span>Wrap in<\/span>/);
    // An empty submenu cannot be opened.
    expect(markup).toMatch(/disabled=""[^>]*><span>Change type<\/span>/);
    expect(markup).toContain('class="context-menu-item danger"');
    // Submenus render only when opened.
    expect(markup).not.toContain('Inverter');
  });
});
