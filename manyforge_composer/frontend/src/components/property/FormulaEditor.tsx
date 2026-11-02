import { useEffect, useRef, useState } from 'react';

import { api } from '../../api/client';
import type {
  ExpressionCheckResult,
  ExpressionDiagnostic,
  ExpressionValueType,
} from '../../types/api';
import { OverlayPanel } from '../OverlayPanel';

/**
 * The formula editor (docs/authoring/EXPRESSIONS.md §10): the backend checks
 * the text while it is typed and returns diagnostics at their positions, the
 * result type, a preview from authoring values and the names a formula may
 * use. The page has no expression engine of its own.
 */

const CHECK_DELAY_MS = 250;

/** The backend's answer for one exact text. */
export type FormulaCheck =
  | { text: string; result: ExpressionCheckResult }
  | { text: string; failure: string };

export interface FormulaEditorState {
  result: ExpressionCheckResult | null;
  /** Why Save is disabled, or null when the text can be saved. */
  saveBlocked: string | null;
}

export function formulaEditorState(text: string, check: FormulaCheck | null): FormulaEditorState {
  const current = check !== null && check.text === text ? check : null;
  const result = current !== null && 'result' in current ? current.result : null;
  let saveBlocked: string | null = null;
  if (text.trim() === '') saveBlocked = 'Enter a formula.';
  else if (current === null) saveBlocked = 'Checking the formula…';
  else if ('failure' in current) saveBlocked = current.failure;
  else if (current.result.diagnostics.length > 0) saveBlocked = 'Fix the errors first.';
  else if (current.result.resultType === null) saveBlocked = 'The formula has no result type.';
  return { result, saveBlocked };
}

/** The text split around one diagnostic's span; positions count Unicode code points. */
export function diagnosticSpan(
  text: string,
  diagnostic: ExpressionDiagnostic,
): { before: string; marked: string; after: string } | null {
  if (diagnostic.position === null || diagnostic.length === null) return null;
  const points = Array.from(text);
  const start = Math.min(diagnostic.position, points.length);
  const end = Math.min(start + diagnostic.length, points.length);
  return {
    before: points.slice(0, start).join(''),
    // A zero-length or end-of-text finding still marks one visible place.
    marked: end > start ? points.slice(start, end).join('') : ' ',
    after: points.slice(end).join(''),
  };
}

export function formatPreviewValue(value: ExpressionCheckResult['preview']): string {
  return 'value' in value
    ? `preview from authoring values: ${typeof value.value === 'string' ? JSON.stringify(value.value) : String(value.value)}`
    : `no preview: ${value.unavailableReason}`;
}

interface FormulaEditorProps {
  title: string;
  expression: string;
  destinationType: ExpressionValueType;
  /** The computed key being edited, so its own formula is not reported as a cycle. */
  excludeComputedKey: string | null;
  onSave: (expression: string) => void;
  onClose: () => void;
  /** The field that opened the editor; it gets the focus back. */
  returnFocus: HTMLElement | null;
}

export function FormulaEditor({
  title,
  expression,
  destinationType,
  excludeComputedKey,
  onSave,
  onClose,
  returnFocus,
}: FormulaEditorProps) {
  const [text, setText] = useState(expression);
  const returnFocusRef = useRef(returnFocus);
  const [check, setCheck] = useState<FormulaCheck | null>(null);
  const textArea = useRef<HTMLTextAreaElement>(null);
  // An empty text is checked too: its answer lists the names to insert.
  useEffect(() => {
    const controller = new AbortController();
    const timer = window.setTimeout(() => {
      api.programWorkspace.checkExpression({
        expression: text,
        destination_type: destinationType,
        exclude_computed_key: excludeComputedKey,
      }, controller.signal).then(
        (result) => setCheck({ text, result }),
        (error: unknown) => {
          if (!controller.signal.aborted) {
            setCheck({ text, failure: error instanceof Error ? error.message : 'The formula check failed.' });
          }
        },
      );
    }, CHECK_DELAY_MS);
    return () => {
      window.clearTimeout(timer);
      controller.abort();
    };
  }, [destinationType, excludeComputedKey, text]);
  const { result, saveBlocked } = formulaEditorState(text, check);
  const names = check !== null && 'result' in check ? check.result.names : [];
  const close = () => {
    onClose();
    window.requestAnimationFrame(() => returnFocusRef.current?.focus());
  };
  const insert = (name: string) => {
    const field = textArea.current;
    const start = field?.selectionStart ?? text.length;
    const end = field?.selectionEnd ?? text.length;
    setText(`${text.slice(0, start)}${name}${text.slice(end)}`);
    window.requestAnimationFrame(() => {
      field?.focus();
      field?.setSelectionRange(start + name.length, start + name.length);
    });
  };
  return (
    <OverlayPanel
      open
      title={title}
      titleId="program-formula-editor-title"
      compact
      onClose={onClose}
      returnFocusRef={returnFocusRef}
    >
      <div className="program-formula-editor">
        <label>
          Formula · result {destinationType}
          <textarea
            ref={textArea}
            rows={3}
            value={text}
            spellCheck={false}
            aria-invalid={(result?.diagnostics.length ?? 0) > 0 || undefined}
            onChange={(event) => setText(event.currentTarget.value)}
          />
        </label>
        {result !== null && text.trim() !== '' && result.diagnostics.length > 0 && (
          <ul className="program-formula-diagnostics" role="alert">
            {result.diagnostics.map((diagnostic, index) => {
              const span = diagnosticSpan(text, diagnostic);
              return (
                <li key={`${diagnostic.code}:${index}`}>
                  {span && <code>{span.before}<mark>{span.marked}</mark>{span.after}</code>}
                  <span>{diagnostic.message}</span>
                </li>
              );
            })}
          </ul>
        )}
        <p className="program-form-hint" role="status">
          {result !== null && text.trim() !== ''
            ? `Result ${result.resultType ?? 'unknown'} · ${formatPreviewValue(result.preview)}`
            : saveBlocked}
        </p>
        {names.length > 0 && (
          <div className="program-formula-names" aria-label="Names to insert">
            {names.map((row) => (
              <button type="button" className="quiet-button" key={row.name} onClick={() => insert(row.name)}>
                <code>{row.name}</code> <small>{row.type}</small>
              </button>
            ))}
          </div>
        )}
        <div className="dialog-actions formula-editor-actions">
          <button type="button" onClick={close}>Cancel</button>
          <button
            type="button"
            className="primary-action"
            disabled={saveBlocked !== null}
            title={saveBlocked ?? undefined}
            onClick={() => {
              onSave(text.trim());
              close();
            }}
          >
            Save
          </button>
        </div>
      </div>
    </OverlayPanel>
  );
}
