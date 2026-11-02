import {
  useEffect,
  useLayoutEffect,
  useRef,
  useState,
  type KeyboardEvent,
} from 'react';

export interface CanvasMenuItem {
  label: string;
  shortcut?: string;
  title?: string;
  disabled?: boolean;
  danger?: boolean;
  onSelect?: () => void;
  /** A submenu; lists longer than SEARCH_THRESHOLD get a filter field. */
  items?: CanvasMenuItem[];
}

export type CanvasMenuEntry = CanvasMenuItem | 'separator';

const SEARCH_THRESHOLD = 10;

interface CanvasContextMenuProps {
  /** Viewport (client) coordinates of the pointer that opened the menu. */
  x: number;
  y: number;
  heading?: string;
  label: string;
  entries: CanvasMenuEntry[];
  onClose: () => void;
}

function focusableItems(list: HTMLElement | null): HTMLButtonElement[] {
  return list === null
    ? []
    : [...list.querySelectorAll<HTMLButtonElement>(':scope > li > button:not(:disabled)')];
}

/** Move focus within one menu list with the arrow, Home and End keys. */
function moveFocus(event: KeyboardEvent<HTMLElement>, list: HTMLElement | null): boolean {
  const items = focusableItems(list);
  if (items.length === 0) return false;
  const current = items.indexOf(document.activeElement as HTMLButtonElement);
  let next: number | null = null;
  if (event.key === 'ArrowDown') next = current < 0 ? 0 : (current + 1) % items.length;
  if (event.key === 'ArrowUp') next = current <= 0 ? items.length - 1 : current - 1;
  if (event.key === 'Home') next = 0;
  if (event.key === 'End') next = items.length - 1;
  if (next === null) return false;
  event.preventDefault();
  items[next]?.focus();
  return true;
}

function MenuList({
  entries,
  label,
  onClose,
  onBack,
  autoFocus,
}: {
  entries: CanvasMenuEntry[];
  label: string;
  onClose: () => void;
  onBack?: () => void;
  autoFocus: boolean;
}) {
  const [openIndex, setOpenIndex] = useState<number | null>(null);
  const [filter, setFilter] = useState('');
  const listRef = useRef<HTMLUListElement>(null);
  const submenuRef = useRef<HTMLDivElement>(null);
  const [flip, setFlip] = useState(false);
  const searchable = onBack !== undefined && entries.length > SEARCH_THRESHOLD;
  const query = filter.trim().toLocaleLowerCase();
  const visible = query === ''
    ? entries
    : entries.filter((entry) => entry !== 'separator'
      && entry.label.toLocaleLowerCase().includes(query));

  useEffect(() => {
    if (!autoFocus) return;
    if (searchable) {
      listRef.current?.parentElement?.querySelector('input')?.focus();
    } else {
      focusableItems(listRef.current)[0]?.focus();
    }
  }, [autoFocus, searchable]);

  // Open a submenu on the left when it would leave the window on the right.
  useLayoutEffect(() => {
    const submenu = submenuRef.current;
    if (submenu === null) return;
    setFlip(submenu.getBoundingClientRect().right > window.innerWidth);
  }, [openIndex]);

  const onKeyDown = (event: KeyboardEvent<HTMLElement>) => {
    if (event.target instanceof HTMLInputElement) {
      // The filter belongs to this list only; never let a parent list act on it.
      event.stopPropagation();
      if (event.key === 'ArrowDown') {
        event.preventDefault();
        focusableItems(listRef.current)[0]?.focus();
      } else if (event.key === 'Escape' || (event.key === 'ArrowLeft' && filter === '')) {
        event.preventDefault();
        onBack?.();
      }
      return;
    }
    if (moveFocus(event, listRef.current)) {
      event.stopPropagation();
      return;
    }
    if (event.key === 'ArrowLeft' || event.key === 'Escape') {
      event.preventDefault();
      event.stopPropagation();
      if (onBack) onBack();
      else onClose();
    }
  };

  return (
    <div className="context-menu-list" onKeyDown={onKeyDown}>
      {searchable && (
        <input
          className="context-menu-filter"
          type="search"
          value={filter}
          placeholder="Filter"
          aria-label={`Filter ${label}`}
          onChange={(event) => setFilter(event.target.value)}
        />
      )}
      <ul ref={listRef} role="menu" aria-label={label}>
        {visible.map((entry, index) => {
          if (entry === 'separator') {
            return <li key={`separator-${index}`} className="context-menu-separator" role="separator" />;
          }
          const hasSubmenu = entry.items !== undefined;
          const open = hasSubmenu && openIndex === index;
          return (
            <li key={`${index}:${entry.label}`} className={open ? 'open' : undefined}>
              <button
                type="button"
                role="menuitem"
                className={entry.danger ? 'context-menu-item danger' : 'context-menu-item'}
                disabled={entry.disabled || (hasSubmenu && entry.items!.length === 0)}
                title={entry.title}
                aria-haspopup={hasSubmenu ? 'menu' : undefined}
                aria-expanded={hasSubmenu ? open : undefined}
                onMouseEnter={() => setOpenIndex(hasSubmenu ? index : null)}
                onKeyDown={(event) => {
                  if (hasSubmenu && (event.key === 'ArrowRight' || event.key === 'Enter' || event.key === ' ')) {
                    event.preventDefault();
                    event.stopPropagation();
                    setOpenIndex(index);
                  }
                }}
                onClick={() => {
                  if (hasSubmenu) {
                    setOpenIndex(index);
                    return;
                  }
                  onClose();
                  entry.onSelect?.();
                }}
              >
                <span>{entry.label}</span>
                {entry.shortcut && <kbd>{entry.shortcut}</kbd>}
                {hasSubmenu && <span className="context-menu-arrow" aria-hidden="true">▸</span>}
              </button>
              {open && (
                <div
                  ref={submenuRef}
                  className={flip ? 'context-menu context-menu-submenu flip' : 'context-menu context-menu-submenu'}
                >
                  <MenuList
                    entries={entry.items!}
                    label={entry.label}
                    onClose={onClose}
                    onBack={() => {
                      setOpenIndex(null);
                      focusableItems(listRef.current)[index]?.focus();
                    }}
                    autoFocus
                  />
                </div>
              )}
            </li>
          );
        })}
      </ul>
    </div>
  );
}

/**
 * A right-click menu at the pointer: closed by Escape, a click elsewhere, or
 * choosing an item. Arrow keys move through items and open submenus.
 */
export function CanvasContextMenu({
  x,
  y,
  heading,
  label,
  entries,
  onClose,
}: CanvasContextMenuProps) {
  const rootRef = useRef<HTMLDivElement>(null);
  const [position, setPosition] = useState({ left: x, top: y });

  // Keep the menu inside the window.
  useLayoutEffect(() => {
    const menu = rootRef.current;
    if (menu === null) return;
    const rect = menu.getBoundingClientRect();
    setPosition({
      left: Math.max(4, Math.min(x, window.innerWidth - rect.width - 4)),
      top: Math.max(4, Math.min(y, window.innerHeight - rect.height - 4)),
    });
  }, [x, y]);

  useEffect(() => {
    const onPointerDown = (event: PointerEvent) => {
      if (!rootRef.current?.contains(event.target as globalThis.Node)) onClose();
    };
    const onBlur = () => onClose();
    document.addEventListener('pointerdown', onPointerDown, true);
    window.addEventListener('blur', onBlur);
    window.addEventListener('resize', onBlur);
    return () => {
      document.removeEventListener('pointerdown', onPointerDown, true);
      window.removeEventListener('blur', onBlur);
      window.removeEventListener('resize', onBlur);
    };
  }, [onClose]);

  return (
    <div
      ref={rootRef}
      className="context-menu"
      style={position}
      onContextMenu={(event) => event.preventDefault()}
    >
      {heading && <strong className="context-menu-heading">{heading}</strong>}
      <MenuList entries={entries} label={label} onClose={onClose} autoFocus />
    </div>
  );
}
