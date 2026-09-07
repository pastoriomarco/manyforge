import { useCallback, useEffect, useRef, useState } from 'react';
import type {
  InputHTMLAttributes,
  KeyboardEvent,
  TextareaHTMLAttributes,
} from 'react';

function formatDraftValue(value: string | number | null | undefined): string {
  return value === null || value === undefined ? '' : String(value);
}

type DraftTextInputProps = Omit<
  InputHTMLAttributes<HTMLInputElement>,
  'value' | 'defaultValue' | 'onChange'
> & {
  value: string | number | null | undefined;
  onCommit: (value: string) => void;
  validate?: (value: string) => string | null;
  onInvalidChange?: (message: string | null) => void;
  onPendingChange?: (pending: boolean) => void;
  resetKey?: string | number;
  shiftStepMultiplier?: number;
  commitNativeStep?: boolean;
  dirtyClassName?: string;
};

export function DraftTextInput({
  value,
  onCommit,
  validate,
  onInvalidChange,
  onPendingChange,
  resetKey,
  shiftStepMultiplier,
  commitNativeStep,
  dirtyClassName,
  onBlur,
  onFocus,
  onKeyDown,
  ...props
}: DraftTextInputProps) {
  const normalizedValue = formatDraftValue(value);
  const [draft, setDraft] = useState(normalizedValue);
  const [focused, setFocused] = useState(false);
  const [invalidMessage, setInvalidMessage] = useState<string | null>(null);
  const skipNextCommit = useRef(false);
  const inputRef = useRef<HTMLInputElement | null>(null);
  const sawBeforeInput = useRef(false);
  const userEdited = useRef(false);

  useEffect(() => {
    if (!userEdited.current || (!focused && invalidMessage === null)) {
      userEdited.current = false;
      setDraft(normalizedValue);
    } else if (draft === normalizedValue && invalidMessage === null) {
      userEdited.current = false;
    }
  }, [draft, focused, invalidMessage, normalizedValue]);

  useEffect(() => {
    onPendingChange?.(invalidMessage !== null || (userEdited.current && draft !== normalizedValue));
  }, [draft, invalidMessage, normalizedValue, onPendingChange]);

  const currentValue = useRef(normalizedValue);
  currentValue.current = normalizedValue;
  useEffect(() => {
    userEdited.current = false;
    setDraft(currentValue.current);
    setInvalidMessage(null);
    onInvalidChange?.(null);
    onPendingChange?.(false);
    // Only an explicit local rebase resets even an invalid/focused input.
    // Ordinary owner observations must not overwrite what is being typed.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [resetKey]);

  const validateDraft = useCallback(() => {
    const message = validate?.(draft) ?? null;
    setInvalidMessage(message);
    onInvalidChange?.(message);
    return message;
  }, [draft, onInvalidChange, validate]);
  const commit = useCallback(() => {
    if (!userEdited.current) return;
    const message = validateDraft();
    if (message === null && draft !== normalizedValue) onCommit(draft);
  }, [draft, normalizedValue, onCommit, validateDraft]);
  const reset = useCallback(() => {
    userEdited.current = false;
    setDraft(normalizedValue);
    setInvalidMessage(null);
    onInvalidChange?.(null);
    onPendingChange?.(false);
  }, [normalizedValue, onInvalidChange, onPendingChange]);

  useEffect(() => {
    const input = inputRef.current;
    if (!commitNativeStep || input === null) return undefined;
    const onBeforeInput = () => {
      sawBeforeInput.current = true;
    };
    const onInput = (event: Event) => {
      const inputEvent = event as InputEvent;
      const typedEdit = sawBeforeInput.current || Boolean(inputEvent.inputType);
      sawBeforeInput.current = false;
      if (typedEdit) return;
      const candidate = input.value;
      userEdited.current = true;
      const message = validate?.(candidate) ?? null;
      setDraft(candidate);
      setInvalidMessage(message);
      onInvalidChange?.(message);
      if (message === null && candidate !== normalizedValue) onCommit(candidate);
    };
    input.addEventListener('beforeinput', onBeforeInput);
    input.addEventListener('input', onInput);
    return () => {
      input.removeEventListener('beforeinput', onBeforeInput);
      input.removeEventListener('input', onInput);
    };
  }, [commitNativeStep, normalizedValue, onCommit, onInvalidChange, validate]);

  return (
    <input
      ref={inputRef}
      {...props}
      className={`${props.className ?? ''}${
        dirtyClassName && draft !== normalizedValue ? ` ${dirtyClassName}` : ''
      }${invalidMessage ? ' draft-input-invalid' : ''}`.trim()}
      aria-invalid={invalidMessage ? true : props['aria-invalid']}
      title={invalidMessage ?? props.title}
      value={draft}
      onFocus={(event) => {
        setFocused(true);
        onFocus?.(event);
      }}
      onChange={(event) => {
        const next = event.target.value;
        userEdited.current = true;
        setDraft(next);
        onPendingChange?.(next !== normalizedValue || (validate?.(next) ?? null) !== null);
      }}
      onBlur={(event) => {
        if (skipNextCommit.current) skipNextCommit.current = false;
        else commit();
        setFocused(false);
        onBlur?.(event);
      }}
      onKeyDown={(event: KeyboardEvent<HTMLInputElement>) => {
        if (
          shiftStepMultiplier !== undefined
          && (event.key === 'ArrowUp' || event.key === 'ArrowDown')
          && props.type === 'number'
        ) {
          const current = Number(draft);
          const baseStep = typeof props.step === 'number'
            ? props.step
            : Number(props.step ?? 1);
          if (Number.isFinite(current) && Number.isFinite(baseStep)) {
            const direction = event.key === 'ArrowUp' ? 1 : -1;
            const minimum = props.min === undefined ? -Infinity : Number(props.min);
            const maximum = props.max === undefined ? Infinity : Number(props.max);
            const next = String(Math.min(maximum, Math.max(
              minimum,
              current + direction * baseStep * (
                event.shiftKey ? shiftStepMultiplier : 1
              ),
            )));
            userEdited.current = true;
            setDraft(next);
            onPendingChange?.(next !== normalizedValue || (validate?.(next) ?? null) !== null);
            if (commitNativeStep) {
              const message = validate?.(next) ?? null;
              setInvalidMessage(message);
              onInvalidChange?.(message);
              if (message === null && next !== normalizedValue) onCommit(next);
            }
            event.preventDefault();
            return;
          }
        }
        if (event.key === 'Enter' || event.key === 'Tab') {
          if (validateDraft() !== null) {
            event.preventDefault();
            return;
          }
        }
        if (event.key === 'Enter') {
          event.preventDefault();
          event.currentTarget.blur();
          return;
        }
        if (event.key === 'Escape') {
          event.preventDefault();
          skipNextCommit.current = true;
          reset();
          event.currentTarget.blur();
          return;
        }
        onKeyDown?.(event);
      }}
    />
  );
}

type DraftTextareaProps = Omit<
  TextareaHTMLAttributes<HTMLTextAreaElement>,
  'value' | 'defaultValue' | 'onChange'
> & {
  value: string | number | null | undefined;
  onCommit: (value: string) => void;
  validate?: (value: string) => string | null;
  onInvalidChange?: (message: string | null) => void;
};

export function DraftTextarea({
  value,
  onCommit,
  validate,
  onInvalidChange,
  onBlur,
  onFocus,
  onKeyDown,
  ...props
}: DraftTextareaProps) {
  const normalizedValue = formatDraftValue(value);
  const [draft, setDraft] = useState(normalizedValue);
  const [focused, setFocused] = useState(false);
  const [invalidMessage, setInvalidMessage] = useState<string | null>(null);
  const skipNextCommit = useRef(false);

  useEffect(() => {
    if (!focused && invalidMessage === null) setDraft(normalizedValue);
  }, [focused, invalidMessage, normalizedValue]);

  const validateDraft = useCallback(() => {
    const message = validate?.(draft) ?? null;
    setInvalidMessage(message);
    onInvalidChange?.(message);
    return message;
  }, [draft, onInvalidChange, validate]);
  const commit = useCallback(() => {
    const message = validateDraft();
    if (message === null && draft !== normalizedValue) onCommit(draft);
  }, [draft, normalizedValue, onCommit, validateDraft]);
  const reset = useCallback(() => {
    setDraft(normalizedValue);
    setInvalidMessage(null);
    onInvalidChange?.(null);
  }, [normalizedValue, onInvalidChange]);

  return (
    <textarea
      {...props}
      className={`${props.className ?? ''}${invalidMessage ? ' draft-input-invalid' : ''}`.trim()}
      aria-invalid={invalidMessage ? true : props['aria-invalid']}
      title={invalidMessage ?? props.title}
      value={draft}
      onFocus={(event) => {
        setFocused(true);
        onFocus?.(event);
      }}
      onChange={(event) => setDraft(event.target.value)}
      onBlur={(event) => {
        if (skipNextCommit.current) skipNextCommit.current = false;
        else commit();
        setFocused(false);
        onBlur?.(event);
      }}
      onKeyDown={(event: KeyboardEvent<HTMLTextAreaElement>) => {
        if (event.key === 'Tab' && validateDraft() !== null) {
          event.preventDefault();
          return;
        }
        if ((event.ctrlKey || event.metaKey) && event.key === 'Enter') {
          if (validateDraft() !== null) {
            event.preventDefault();
            return;
          }
          event.preventDefault();
          event.currentTarget.blur();
          return;
        }
        if (event.key === 'Escape') {
          event.preventDefault();
          skipNextCommit.current = true;
          reset();
          event.currentTarget.blur();
          return;
        }
        onKeyDown?.(event);
      }}
    />
  );
}
