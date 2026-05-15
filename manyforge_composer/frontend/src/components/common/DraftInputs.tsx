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
};

export function DraftTextInput({
  value,
  onCommit,
  onBlur,
  onFocus,
  onKeyDown,
  ...props
}: DraftTextInputProps) {
  const normalizedValue = formatDraftValue(value);
  const [draft, setDraft] = useState(normalizedValue);
  const [focused, setFocused] = useState(false);
  const skipNextCommit = useRef(false);

  useEffect(() => {
    if (!focused) {
      setDraft(normalizedValue);
    }
  }, [focused, normalizedValue]);

  const commit = useCallback(() => {
    if (draft !== normalizedValue) {
      onCommit(draft);
    }
  }, [draft, normalizedValue, onCommit]);

  const reset = useCallback(() => {
    setDraft(normalizedValue);
  }, [normalizedValue]);

  return (
    <input
      {...props}
      value={draft}
      onFocus={(event) => {
        setFocused(true);
        onFocus?.(event);
      }}
      onChange={(event) => setDraft(event.target.value)}
      onBlur={(event) => {
        if (skipNextCommit.current) {
          skipNextCommit.current = false;
        } else {
          commit();
        }
        setFocused(false);
        onBlur?.(event);
      }}
      onKeyDown={(event: KeyboardEvent<HTMLInputElement>) => {
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
};

export function DraftTextarea({
  value,
  onCommit,
  onBlur,
  onFocus,
  onKeyDown,
  ...props
}: DraftTextareaProps) {
  const normalizedValue = formatDraftValue(value);
  const [draft, setDraft] = useState(normalizedValue);
  const [focused, setFocused] = useState(false);
  const skipNextCommit = useRef(false);

  useEffect(() => {
    if (!focused) {
      setDraft(normalizedValue);
    }
  }, [focused, normalizedValue]);

  const commit = useCallback(() => {
    if (draft !== normalizedValue) {
      onCommit(draft);
    }
  }, [draft, normalizedValue, onCommit]);

  const reset = useCallback(() => {
    setDraft(normalizedValue);
  }, [normalizedValue]);

  return (
    <textarea
      {...props}
      value={draft}
      onFocus={(event) => {
        setFocused(true);
        onFocus?.(event);
      }}
      onChange={(event) => setDraft(event.target.value)}
      onBlur={(event) => {
        if (skipNextCommit.current) {
          skipNextCommit.current = false;
        } else {
          commit();
        }
        setFocused(false);
        onBlur?.(event);
      }}
      onKeyDown={(event: KeyboardEvent<HTMLTextAreaElement>) => {
        if ((event.ctrlKey || event.metaKey) && event.key === 'Enter') {
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
