import { Fragment, useCallback, useState, type ReactNode } from 'react';

import type { KeyNamespaceGroup } from '../../utils/blackboardNamespaces';

const TOGGLE_PREFIX = '__namespace__:';

/**
 * Collapse state of a native key select's namespace groups (component state
 * only). `handleToggle` consumes a group-header choice and reports it.
 */
export function useNamespaceGroups() {
  const [expanded, setExpanded] = useState<ReadonlySet<string>>(() => new Set());
  const handleToggle = useCallback((value: string): boolean => {
    if (!value.startsWith(TOGGLE_PREFIX)) return false;
    const id = value.slice(TOGGLE_PREFIX.length);
    setExpanded((current) => {
      const next = new Set(current);
      if (!next.delete(id)) next.add(id);
      return next;
    });
    return true;
  }, []);
  return { expanded, handleToggle };
}

interface NamespaceGroupOptionsProps<T> {
  groups: KeyNamespaceGroup<T>[];
  expanded: ReadonlySet<string>;
  /** The select's value: its group stays open so the value can show. */
  current: string;
  valueOf: (item: T) => string;
  option: (item: T) => ReactNode;
}

/**
 * Key options grouped by namespace, each group collapsed by default behind a
 * header choice with its count. A single group needs no header.
 */
export function NamespaceGroupOptions<T>({
  groups,
  expanded,
  current,
  valueOf,
  option,
}: NamespaceGroupOptionsProps<T>) {
  if (groups.length === 1) return <>{groups[0].items.map(option)}</>;
  return (
    <>
      {groups.map((group) => {
        const open = expanded.has(group.id)
          || group.items.some((item) => valueOf(item) === current);
        return (
          <Fragment key={group.id}>
            <option value={`${TOGGLE_PREFIX}${group.id}`}>
              {open ? '▾' : '▸'} {group.label} ({group.items.length})
            </option>
            {open && <optgroup label={group.label}>{group.items.map(option)}</optgroup>}
          </Fragment>
        );
      })}
    </>
  );
}
