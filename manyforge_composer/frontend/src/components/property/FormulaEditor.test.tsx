import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';

import type { ExpressionCheckResult } from '../../types/api';
import {
  diagnosticSpan,
  FormulaEditor,
  formatPreviewValue,
  formulaEditorState,
} from './FormulaEditor';

const valid: ExpressionCheckResult = {
  resultType: 'float64',
  diagnostics: [],
  preview: { value: -0.05 },
  names: [{ name: 'param.tube_length', type: 'float64' }],
};
const invalid: ExpressionCheckResult = {
  resultType: null,
  diagnostics: [{ code: 'unknown_name', message: 'Unknown name param.tube', position: 1, length: 10, name: 'param.tube' }],
  preview: { unavailableReason: 'The formula has errors.' },
  names: valid.names,
};

describe('formula editor', () => {
  it('saves only a checked text without diagnostics', () => {
    const text = '-param.tube_length / 2';
    expect(formulaEditorState(text, { text, result: valid })).toEqual({ result: valid, saveBlocked: null });
    expect(formulaEditorState('-param.tube / 2', { text: '-param.tube / 2', result: invalid }).saveBlocked)
      .toBe('Fix the errors first.');
    // An answer for an earlier text never enables Save for the current one.
    expect(formulaEditorState(`${text} + 1`, { text, result: valid })).toEqual({
      result: null, saveBlocked: 'Checking the formula…',
    });
    expect(formulaEditorState(text, { text, failure: 'Composer is unavailable.' }).saveBlocked)
      .toBe('Composer is unavailable.');
    expect(formulaEditorState('  ', null).saveBlocked).toBe('Enter a formula.');
    expect(formulaEditorState(text, { text, result: { ...valid, resultType: null } }).saveBlocked)
      .toBe('The formula has no result type.');
  });

  it('marks a diagnostic at its code-point position', () => {
    expect(diagnosticSpan('-param.tube / 2', invalid.diagnostics[0]!)).toEqual({
      before: '-', marked: 'param.tube', after: ' / 2',
    });
    // Positions count code points, not UTF-16 units.
    expect(diagnosticSpan('"é😀" == x', { ...invalid.diagnostics[0]!, position: 3, length: 1 }))
      .toEqual({ before: '"é😀', marked: '"', after: ' == x' });
    expect(diagnosticSpan('1 +', { ...invalid.diagnostics[0]!, position: 3, length: 0 }))
      .toEqual({ before: '1 +', marked: ' ', after: '' });
    expect(diagnosticSpan('a', { ...invalid.diagnostics[0]!, position: null, length: null })).toBeNull();
  });

  it('labels the preview as computed from authoring values, never as a runtime value', () => {
    expect(formatPreviewValue(valid.preview)).toBe('preview from authoring values: -0.05');
    expect(formatPreviewValue({ value: 'lite6' })).toBe('preview from authoring values: "lite6"');
    expect(formatPreviewValue(invalid.preview)).toBe('no preview: The formula has errors.');
  });

  it('opens with the current formula and Save disabled until the backend has checked it', () => {
    const markup = renderToStaticMarkup(
      <FormulaEditor
        title="Formula for goal.position_m[2]"
        expression="-param.tube_length / 2"
        destinationType="float64"
        excludeComputedKey={null}
        onSave={vi.fn()}
        onClose={vi.fn()}
        returnFocus={null}
      />,
    );
    expect(markup).toContain('Formula · result float64');
    expect(markup).toContain('-param.tube_length / 2</textarea>');
    expect(markup).toContain('Checking the formula…');
    expect(markup).toMatch(/class="primary-action" disabled=""/);
  });
});
