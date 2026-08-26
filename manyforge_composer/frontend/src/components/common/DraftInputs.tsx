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
};

export function DraftTextInput({
  value,
  onCommit,
  validate,
  onInvalidChange,
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
    <input
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
      onKeyDown={(event: KeyboardEvent<HTMLInputElement>) => {
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
