import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';

import { ProgramNoticeRail, type ProgramNotice } from './ProgramNoticeRail';
import {
  mergeProgramNotice,
  PROGRAM_INFO_NOTICE_TIMEOUT_MS,
} from './ProgramWorkspace';

function notice(id: string, kind: 'info' | 'error' = 'info'): ProgramNotice {
  return {
    id,
    key: id,
    kind,
    message: `Notice ${id}`,
    dismissible: true,
  };
}

describe('Program canvas notice rail', () => {
  it('coalesces stable keys and retains only the bounded newest notices', () => {
    const first = notice('same');
    const updated = { ...first, message: 'Updated', kind: 'error' as const };
    expect(mergeProgramNotice([first], updated)).toEqual([updated]);

    let notices: ProgramNotice[] = [];
    for (let index = 0; index < 10; index += 1) {
      notices = mergeProgramNotice(notices, notice(String(index)));
    }
    expect(notices.map((row) => row.id)).toEqual(['2', '3', '4', '5', '6', '7', '8', '9']);
    expect(PROGRAM_INFO_NOTICE_TIMEOUT_MS).toBe(4_000);
  });

  it('renders one compact dismissible live row and one integrated multiple stack', () => {
    const single = renderToStaticMarkup(
      <ProgramNoticeRail
        notices={[notice('one')]}
        onDismiss={vi.fn()}
        onDismissAll={vi.fn()}
      />,
    );
    expect(single).toContain('role="status"');
    expect(single).toContain('Dismiss Program notice');
    expect(single).not.toContain('Dismiss all');

    const multiple = renderToStaticMarkup(
      <ProgramNoticeRail
        notices={[notice('one'), notice('two', 'error')]}
        onDismiss={vi.fn()}
        onDismissAll={vi.fn()}
      />,
    );
    expect(multiple).toContain('2 notices');
    expect(multiple).toContain('aria-expanded="false"');
    expect(multiple).toContain('Dismiss all');
    expect(multiple).toContain('role="alert"');
    expect(multiple).toContain('2 notices · Notice two');
  });
});
