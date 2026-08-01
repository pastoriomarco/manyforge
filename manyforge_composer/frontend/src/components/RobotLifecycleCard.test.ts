/// <reference types="node" />

import { createElement } from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';
import { RobotLifecycleCard } from './RobotLifecycleCard';

describe('robot lifecycle observation card', () => {
  it('shows supplied facts without rendering command controls', () => {
    const html = renderToStaticMarkup(createElement(RobotLifecycleCard, {
      status: {
        resourceId: 'robot-1',
        readiness: 'waiting',
        fresh: true,
        userMessage: 'Drives are disabled.',
        facts: [
          { id: 'transport', label: 'Connection', value: 'true' },
          { id: 'drives', label: 'Drives', value: 'false' },
        ],
      },
    }));
    expect(html).toContain('robot-1');
    expect(html).toContain('Drives are disabled.');
    expect(html).not.toContain('Enable robot');
    expect(html).not.toContain('Disable robot');
  });
});
