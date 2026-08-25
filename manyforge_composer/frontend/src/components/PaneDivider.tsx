import { useRef } from 'react';
import type {
  KeyboardEvent as ReactKeyboardEvent,
  PointerEvent as ReactPointerEvent,
} from 'react';

interface PaneDividerProps {
  label: string;
  value: number;
  min: number;
  max: number;
  direction?: 1 | -1;
  onChange: (value: number) => void;
  className?: string;
}

function clamp(value: number, min: number, max: number): number {
  return Math.min(Math.max(value, min), max);
}

/** Pointer- and keyboard-operable presentation-only horizontal pane sizing. */
export function PaneDivider({
  label,
  value,
  min,
  max,
  direction = 1,
  onChange,
  className = '',
}: PaneDividerProps) {
  const pointer = useRef<{ id: number; startX: number; startValue: number } | null>(null);

  const handlePointerDown = (event: ReactPointerEvent<HTMLDivElement>) => {
    event.preventDefault();
    event.currentTarget.setPointerCapture(event.pointerId);
    pointer.current = {
      id: event.pointerId,
      startX: event.clientX,
      startValue: value,
    };
  };
  const handlePointerMove = (event: ReactPointerEvent<HTMLDivElement>) => {
    const active = pointer.current;
    if (active === null || active.id !== event.pointerId) return;
    onChange(clamp(
      active.startValue + direction * (event.clientX - active.startX),
      min,
      max,
    ));
  };
  const finishPointer = (event: ReactPointerEvent<HTMLDivElement>) => {
    if (pointer.current?.id !== event.pointerId) return;
    if (event.currentTarget.hasPointerCapture(event.pointerId)) {
      event.currentTarget.releasePointerCapture(event.pointerId);
    }
    pointer.current = null;
  };
  const handleKeyDown = (event: ReactKeyboardEvent<HTMLDivElement>) => {
    const step = event.shiftKey ? 50 : 10;
    const next = event.key === 'ArrowLeft'
      ? value - direction * step
      : event.key === 'ArrowRight'
        ? value + direction * step
        : event.key === 'Home'
          ? min
          : event.key === 'End'
            ? max
            : null;
    if (next === null) return;
    event.preventDefault();
    onChange(clamp(next, min, max));
  };

  return (
    <div
      className={`pane-divider${className ? ` ${className}` : ''}`}
      role="separator"
      tabIndex={0}
      aria-label={`Resize ${label} pane`}
      aria-orientation="vertical"
      aria-valuemin={min}
      aria-valuemax={max}
      aria-valuenow={value}
      aria-valuetext={`${value} pixels`}
      title={`Resize ${label} pane`}
      onPointerDown={handlePointerDown}
      onPointerMove={handlePointerMove}
      onPointerUp={finishPointer}
      onPointerCancel={finishPointer}
      onLostPointerCapture={() => {
        pointer.current = null;
      }}
      onKeyDown={handleKeyDown}
    />
  );
}
