import {
  createContext,
  useContext,
  useEffect,
  useState,
  type KeyboardEvent,
  type MouseEvent,
  type ReactNode,
} from 'react';

import { api } from '../../api/client';
import type { CatalogJsonSchema, ExpressionValueType, ProgramValueSource } from '../../types/api';
import { groupByNamespace } from '../../utils/blackboardNamespaces';
import { sameValueSource, type ValueSourceMenuModel } from '../../utils/valueSourceMenu';
import type { CanvasMenuEntry } from '../CanvasContextMenu';
import { formatPreviewValue } from './FormulaEditor';

/**
 * Where a value comes from, at any depth of a node's parameters
 * (docs/authoring/EXPRESSIONS.md §10). Editors mark each value that may come
 * from a key, a parameter or a formula with its path; the Program property
 * editor provides the controller that owns the menu, the formula editor and
 * the Draft mutations. Without a controller (for example the runtime
 * blackboard editor) the fields render as plain inputs.
 */
export interface ValueSourceController {
  model: (path: string, schema: CatalogJsonSchema) => ValueSourceMenuModel | null;
  /** Open the menu for `path` at a pointer position, or under `anchor` for the keyboard. */
  openMenu: (
    path: string,
    schema: CatalogJsonSchema,
    anchor: HTMLElement,
    point: { x: number; y: number } | null,
  ) => void;
  editFormula: (path: string, schema: CatalogJsonSchema) => void;
  /** The chip or field text of a source and its tooltip. */
  describe: (source: ProgramValueSource) => { label: string; title: string };
  hasSourcesWithin: (path: string) => boolean;
  disabled: boolean;
}

export const ValueSourceContext = createContext<ValueSourceController | null>(null);

export function useValueSources(): ValueSourceController | null {
  return useContext(ValueSourceContext);
}

export interface ValueSourceMenuActions {
  typeValues: () => void;
  use: (source: ProgramValueSource) => void;
  editFormula: () => void;
}

/** The menu entries of one value: always the same order, disabled rather than hidden. */
export function valueSourceMenuEntries(
  model: ValueSourceMenuModel,
  actions: ValueSourceMenuActions,
): CanvasMenuEntry[] {
  const unavailable = model.unavailable !== null;
  const choices = (rows: ValueSourceMenuModel['keyChoices']) => rows.map((choice) => {
    const current = sameValueSource(model.current, choice.source);
    return {
      label: choice.label,
      disabled: current,
      title: current ? 'Current source' : undefined,
      onSelect: () => actions.use(choice.source),
    };
  });
  // Keys are grouped by namespace, one submenu each (closed until opened).
  const keyGroups = groupByNamespace(
    model.keyChoices,
    (choice) => ('reference' in choice.source ? choice.source.reference : ''),
  );
  const keyItems = keyGroups.length === 1
    ? choices(model.keyChoices)
    : keyGroups.map((group) => ({
      label: `${group.label} (${group.items.length})`,
      items: choices(group.items),
    }));
  const entries: CanvasMenuEntry[] = [
    {
      label: 'Type values',
      disabled: model.current === undefined,
      title: model.current === undefined ? 'The value is typed here.' : 'Return to the stored value.',
      onSelect: actions.typeValues,
    },
    {
      label: 'Use key…',
      disabled: unavailable,
      title: model.unavailable ?? (model.keyChoices.length === 0 ? 'No readable key of this type is declared.' : undefined),
      items: keyItems,
    },
  ];
  if (!model.keyOnly) entries.push({
    label: 'Use parameter…',
    disabled: unavailable,
    title: model.unavailable ?? (model.parameterChoices.length === 0 ? 'No parameter of this type is declared.' : undefined),
    items: choices(model.parameterChoices),
  });
  if (model.formulaType === null) return entries;
  entries.push('separator');
  if (model.current?.kind === 'expression') {
    entries.push(
      { label: 'Edit formula', onSelect: actions.editFormula },
      { label: 'Remove formula', onSelect: actions.typeValues },
    );
  } else {
    entries.push({
      label: 'Add formula',
      disabled: unavailable,
      title: model.unavailable ?? `A formula producing ${model.formulaType}.`,
      onSelect: actions.editFormula,
    });
  }
  return entries;
}

function menuHandlers(
  controller: ValueSourceController,
  path: string,
  schema: CatalogJsonSchema,
) {
  return {
    onContextMenu: (event: MouseEvent<HTMLElement>) => {
      if (controller.disabled) return;
      event.preventDefault();
      event.stopPropagation();
      // A keyboard-opened context menu may report no pointer position.
      const pointer = event.clientX !== 0 || event.clientY !== 0;
      controller.openMenu(path, schema, event.currentTarget, pointer
        ? { x: event.clientX, y: event.clientY }
        : null);
    },
    onKeyDown: (event: KeyboardEvent<HTMLElement>) => {
      if (controller.disabled) return;
      if (event.key !== 'ContextMenu' && !(event.key === 'F10' && event.shiftKey)) return;
      event.preventDefault();
      event.stopPropagation();
      controller.openMenu(path, schema, event.currentTarget, null);
    },
  };
}

/** The last preview of a formula, for its field's tooltip. */
function useFormulaPreview(expression: string | null, type: ExpressionValueType | null): string | null {
  const [preview, setPreview] = useState<{ key: string; text: string } | null>(null);
  const key = expression === null || type === null ? null : `${type}:${expression}`;
  useEffect(() => {
    if (expression === null || type === null || key === null) return undefined;
    const controller = new AbortController();
    api.programWorkspace.checkExpression(
      { expression, destination_type: type, exclude_computed_key: null },
      controller.signal,
    ).then((result) => setPreview({
      key,
      text: result.diagnostics.length > 0
        ? result.diagnostics.map((diagnostic) => diagnostic.message).join(' ')
        : formatPreviewValue(result.preview),
    }), () => undefined);
    return () => controller.abort();
  }, [expression, key, type]);
  return preview !== null && preview.key === key ? preview.text : null;
}

/**
 * A whole value (a pose, a path, a parameter): its fields while typed, one
 * compact chip while it comes from a key or a parameter.
 */
export function SourceableValue({
  path,
  schema,
  children,
}: {
  path: string;
  schema: CatalogJsonSchema;
  children: ReactNode;
}) {
  const controller = useValueSources();
  const model = controller?.model(path, schema) ?? null;
  if (controller === null || model === null) return children;
  const handlers = menuHandlers(controller, path, schema);
  if (model.current !== undefined && model.current.kind !== 'expression') {
    const { label, title } = controller.describe(model.current);
    return (
      <button
        type="button"
        className="program-value-chip"
        title={title}
        disabled={controller.disabled}
        onClick={(event) => controller.openMenu(path, schema, event.currentTarget, null)}
        {...handlers}
      >
        {label}
      </button>
    );
  }
  return <div className="program-value-scope" {...handlers}>{children}</div>;
}

/**
 * One field (a number, a bool, a string). A formula or a binding shows in the
 * field's own box, with `f(x)` at its trailing edge for a formula, so the
 * field keeps its size and position.
 */
export function SourceableLeaf({
  path,
  schema,
  label,
  children,
}: {
  path: string;
  schema: CatalogJsonSchema;
  label: string;
  children: ReactNode;
}) {
  const controller = useValueSources();
  const model = controller?.model(path, schema) ?? null;
  const expression = model?.current?.kind === 'expression' ? model.current.expression : null;
  const preview = useFormulaPreview(expression, model?.formulaType ?? null);
  if (controller === null || model === null) return children;
  const handlers = menuHandlers(controller, path, schema);
  if (model.current === undefined) {
    return <span className="program-value-field" {...handlers}>{children}</span>;
  }
  const { label: text, title } = controller.describe(model.current);
  const open = (anchor: HTMLElement) => expression === null
    ? controller.openMenu(path, schema, anchor, null)
    : controller.editFormula(path, schema);
  return (
    <span className="program-value-field sourced" {...handlers}>
      <input
        readOnly
        value={text}
        aria-label={`${label}: ${expression === null ? text : `formula ${text}`}`}
        title={preview === null ? title : `${title}\n${preview}`}
        disabled={controller.disabled}
        onClick={(event) => open(event.currentTarget)}
        onKeyDown={(event) => {
          if (event.key !== 'Enter') return;
          event.preventDefault();
          open(event.currentTarget);
        }}
      />
      {expression !== null && (
        <button
          type="button"
          className="program-value-marker"
          tabIndex={-1}
          disabled={controller.disabled}
          aria-label={`Edit the formula of ${label}`}
          onClick={() => controller.editFormula(path, schema)}
        >
          f(x)
        </button>
      )}
    </span>
  );
}
